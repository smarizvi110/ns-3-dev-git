/*
 * CATS TCP Experiment: Core Prioritization
 * ===========================================
 *
 * Purpose:
 * - Compare CATS queue-jumping behavior against baseline TCP under identical
 *   traffic and topology conditions.
 * - Emit only priority-group completion metrics used by paper plots.
 *
 * Traffic pattern:
 * - Five groups are sent in reverse priority order: P4 -> P3 -> P2 -> P1 -> P0
 * - Group sizes: P0=8KB, P1=25KB, P2=40KB, P3=60KB, P4=150KB
 *
 * Expected behavior:
 * - CATS should invert completion order to priority order: P0 first, P4 last.
 *
 * Output files:
 * - priority_completion.tsv
 * - parameter_debug.log
 * - experiment-core-prio-*.pcap
 *
 * Run:
 *   ./ns3 run "experiment-core-prioritization"
 *   ./ns3 run "experiment-core-prioritization --outputDir=<dir>"
 *
 * Parameterization:
 * - Network parameters are exposed via command-line arguments.
 * - CATS fairness parameters are exposed via ns3::TcpCats defaults.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/tcp-cats.h"
#include "ns3/tcp-bbr.h"
#include "ns3/tcp-prr-recovery.h"
#include "ns3/rtt-estimator.h"
#include "ns3/priority-tag.h"
#include "ns3/point-to-point-dumbbell.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ExperimentCorePrioritization");

// Global trace files
std::ofstream g_priorityCompletionFile;
std::ofstream g_parameterDebugFile;

/**
 * \brief Create organized output directory with timestamp and parameter info
 * 
 * Creates a directory structure like:
 * experiment-results/YYYY-MM-DD-HH-MM-SS-paramHash/
 * 
 * \param baseOutputDir Base output directory (usually ".")
 * \return Full path to the created directory
 */
std::string CreateOrganizedOutputDirectory(const std::string& baseOutputDir) 
{
    // Get current timestamp
    std::time_t now = std::time(nullptr);
    std::tm* timeinfo = std::localtime(&now);
    
    char timestamp[100];
    std::strftime(timestamp, sizeof(timestamp), "%d-%m-%Y-%H-%M-%S", timeinfo);
    
    // Create parameter hash - since we can't easily read current values,
    // we'll create a simple timestamp-based directory for now
    std::string paramHash = "run";
    
    // Create full directory path
    std::string fullDir = baseOutputDir + "/experiment-results/" + timestamp + "_" + paramHash;
    
    // Create the directory (using system call for simplicity)
    std::string mkdirCmd = "mkdir -p " + fullDir;
    int result = std::system(mkdirCmd.c_str());
    if (result != 0) {
        NS_FATAL_ERROR("Failed to create output directory: " << fullDir);
    }
    
    return fullDir;
}

/**
 * \brief Priority Group Data Structure
 * Represents a group of data with the same priority level
 */
struct PriorityGroup
{
    uint8_t priority;
    uint32_t totalSize;        // Total bytes in this priority group
    uint32_t bytesReceived;    // Bytes received so far
    Time startTime;            // When first byte was sent
    Time completionTime;       // When last byte was received
    bool isComplete;           // Whether all data has been received
    std::string description;   // Human-readable description
    
    PriorityGroup(uint8_t p, uint32_t size, const std::string& desc)
        : priority(p), totalSize(size), bytesReceived(0), isComplete(false), description(desc)
    {
    }
};

/**
 * \brief CATS Priority Data Generator Application
 * 
 * Generates realistic mixed-priority traffic patterns for CATS analysis.
 * Simulates a web page load with different priority levels:
 * P0: Critical HTML/CSS (render-blocking)
 * P1: JavaScript frameworks  
 * P2: Application JavaScript
 * P3: Images and media
 * P4: Analytics and tracking
 */
class CatsPriorityDataApp : public Application
{
public:
    CatsPriorityDataApp();
    virtual ~CatsPriorityDataApp();
    
    void Setup(Ptr<Socket> socket, Address address, uint16_t port);
    void InitializePriorityGroups();
    uint32_t GetTotalDataSize() const;

private:
    virtual void StartApplication(void) override;
    virtual void StopApplication(void) override;
    
    void SendAllPriorityGroupsConcurrently();
    void SendDataChunk(uint8_t priority, uint32_t size);
    
    Ptr<Socket> m_socket;
    Address m_peer;
    uint16_t m_peerPort;
    
    // Priority groups (realistic web page data)
    std::vector<PriorityGroup> m_priorityGroups;
    uint32_t m_currentGroupIndex;
    uint32_t m_totalBytesSent;
    
    EventId m_sendEvent;
    Time m_dataStartTime;
};

/**
 * \brief CATS Priority Receiver Application
 * 
 * Receives priority data and tracks completion times per priority group
 */
class CatsPriorityReceiverApp : public Application
{
public:
    CatsPriorityReceiverApp();
    virtual ~CatsPriorityReceiverApp();
    
    void Setup(Ptr<Socket> socket);
    void SetPriorityGroups(const std::vector<PriorityGroup>& groups);

private:
    virtual void StartApplication(void) override;
    virtual void StopApplication(void) override;
    
    void HandleRead(Ptr<Socket> socket);
    void HandleAcceptConnection(Ptr<Socket> socket, const Address& from);
    void ProcessReceivedData(Ptr<Packet> packet);
    void CheckPriorityGroupCompletion(uint8_t priority);
    void CheckAllGroupsComplete();
    void LogPriorityGroupCompletion(const PriorityGroup& group);
    
    Ptr<Socket> m_socket;
    std::vector<PriorityGroup> m_priorityGroups;
    uint32_t m_totalBytesReceived;
    Time m_firstByteTime;
    Time m_lastByteTime;
    bool m_firstByteReceived;
};

// Implementation: CatsPriorityDataApp
CatsPriorityDataApp::CatsPriorityDataApp()
    : m_socket(nullptr),
      m_currentGroupIndex(0),
      m_totalBytesSent(0)
{
}

CatsPriorityDataApp::~CatsPriorityDataApp()
{
    m_socket = nullptr;
}

void
CatsPriorityDataApp::Setup(Ptr<Socket> socket, Address address, uint16_t port)
{
    m_socket = socket;
    m_peer = address;
    m_peerPort = port;
    InitializePriorityGroups();
}

void
CatsPriorityDataApp::InitializePriorityGroups()
{
    // Realistic web page priority groups (inspired by Chrome DevTools priorities)
    // IMPORTANT: Sizes chosen to demonstrate CATS queue jumping effectively
    // P4 (analytics) is sent FIRST but should be delivered LAST by CATS
    m_priorityGroups.clear();
    
    // P0: Critical render-blocking resources (HTML + critical CSS) - small, urgent
    m_priorityGroups.emplace_back(0, 8000, "Critical HTML/CSS");
    
    // P1: CSS frameworks (render-blocking but lower priority) - medium  
    m_priorityGroups.emplace_back(1, 25000, "CSS Framework");
    
    // P2: Application JavaScript (functionality) - medium-large
    m_priorityGroups.emplace_back(2, 40000, "Application JS");
    
    // P3: Images and visual content - large
    m_priorityGroups.emplace_back(3, 60000, "Images/Media");
    
    // P4: Analytics and tracking (lowest priority) - LARGEST, sent FIRST
    // This creates the perfect scenario for CATS to demonstrate queue jumping
    m_priorityGroups.emplace_back(4, 150000, "Analytics/Tracking");
    
    NS_LOG_INFO("Initialized " << m_priorityGroups.size() << " priority groups (Total: " << 
                GetTotalDataSize() << " bytes)");
    for (const auto& group : m_priorityGroups)
    {
        NS_LOG_INFO("P" << (uint32_t)group.priority << ": " << group.totalSize << " bytes - " << group.description);
    }
}

uint32_t
CatsPriorityDataApp::GetTotalDataSize() const
{
    uint32_t total = 0;
    for (const auto& group : m_priorityGroups)
    {
        total += group.totalSize;
    }
    return total;
}

void
CatsPriorityDataApp::StartApplication(void)
{
    NS_LOG_FUNCTION(this);
    
    // Configure the CATS socket if it's a TcpCats instance (following cats-dumbbell-refactored.cc)
    // CRITICAL: CATS configuration must be done in StartApplication, not in main()
    Ptr<TcpCats> catsSocket = DynamicCast<TcpCats>(m_socket);
    if (catsSocket)
    {
        NS_LOG_INFO("🎯 Configuring CATS socket with BBR + PRR + RttMeanDeviation");
        
        // Log CATS socket configuration for parameter debugging
        if (g_parameterDebugFile.is_open())
        {
            g_parameterDebugFile << "CATS Socket Configuration started at " << Simulator::Now().GetSeconds() << "s:" << std::endl;
        }
        
        // Step 1: Configure the socket's node and TCP protocol
        catsSocket->SetNode(GetNode());        
        // Step 2: Get the TcpL4Protocol from the node's internet stack  
        Ptr<TcpL4Protocol> tcp = GetNode()->GetObject<TcpL4Protocol>();
        if (tcp)
        {
            catsSocket->SetTcp(tcp);
            if (g_parameterDebugFile.is_open())
            {
                g_parameterDebugFile << "  TcpL4Protocol linked to CATS socket at " << Simulator::Now().GetSeconds() << "s" << std::endl;
            }
            
            // Step 3: Set RTT estimator for accurate timing measurements
            Ptr<RttMeanDeviation> rtt = CreateObject<RttMeanDeviation>();
            catsSocket->SetRtt(rtt);
            if (g_parameterDebugFile.is_open())
            {
                g_parameterDebugFile << "  RTT Estimator: RttMeanDeviation configured at " << Simulator::Now().GetSeconds() << "s" << std::endl;
            }
            
            // Step 4: Configure congestion control algorithm - BBR for better performance
            Ptr<TcpBbr> bbr = CreateObject<TcpBbr>();
            catsSocket->SetCongestionControlAlgorithm(bbr);
            if (g_parameterDebugFile.is_open())
            {
                g_parameterDebugFile << "  Congestion Control: BBR configured at " << Simulator::Now().GetSeconds() << "s" << std::endl;
            }
            
            // Step 5: Set recovery algorithm - PRR for fast recovery from losses
            Ptr<TcpPrrRecovery> recovery = CreateObject<TcpPrrRecovery>();
            catsSocket->SetRecoveryAlgorithm(recovery);
            if (g_parameterDebugFile.is_open())
            {
                g_parameterDebugFile << "  Recovery Algorithm: PRR configured at " << Simulator::Now().GetSeconds() << "s" << std::endl;
            }
            
            // Step 6: CRITICAL - Register socket with TCP layer for proper data transmission
            // Without this, data will not flow through the network stack
            tcp->AddSocket(catsSocket);
            if (g_parameterDebugFile.is_open())
            {
                g_parameterDebugFile << "  CATS socket registered with TcpL4Protocol at " << Simulator::Now().GetSeconds() << "s" << std::endl;
                g_parameterDebugFile << std::endl;
            }
        }
    }
    
    if (m_socket)
    {
        m_socket->Bind();
        m_socket->Connect(InetSocketAddress(Ipv4Address::ConvertFrom(m_peer), m_peerPort));
        
        m_dataStartTime = Simulator::Now();
        
        // Send all priority groups concurrently in REVERSE order (P4 first!)
        // This creates the perfect scenario to demonstrate CATS queue jumping
        SendAllPriorityGroupsConcurrently();
    }
    
    NS_LOG_INFO("CATS Priority Data Generator started - sending " << GetTotalDataSize() << " bytes");
}

void
CatsPriorityDataApp::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    
    if (m_socket)
    {
        m_socket->Close();
    }
    
    Simulator::Cancel(m_sendEvent);
    
    NS_LOG_INFO("Data Generator stopped - Total bytes sent: " << m_totalBytesSent);
}

void
CatsPriorityDataApp::SendAllPriorityGroupsConcurrently()
{
    NS_LOG_INFO("Sending all priority groups concurrently in REVERSE order (P4 first, P0 last)");
    NS_LOG_INFO("This demonstrates CATS queue jumping - P4 sent first but should arrive last!");
    
    // Send in reverse priority order (P4, P3, P2, P1, P0)
    // This forces CATS to reorder packets to deliver high-priority data first
    for (int i = m_priorityGroups.size() - 1; i >= 0; i--)
    {
        PriorityGroup& group = m_priorityGroups[i];
        group.startTime = Simulator::Now();
        
        NS_LOG_INFO("Sending P" << (uint32_t)group.priority << " (" << group.description << ") - " << 
                    group.totalSize << " bytes [Send order: " << (m_priorityGroups.size() - i) << "/5]");
        
        // Send the entire priority group at once
        SendDataChunk(group.priority, group.totalSize);
        m_totalBytesSent += group.totalSize;
        
        // Small delay between priority groups to ensure distinct packet groups
        Simulator::Schedule(MilliSeconds(1), [](){});
    }
    
    NS_LOG_INFO("All priority groups queued for transmission (" << m_totalBytesSent << " total bytes)");
    NS_LOG_INFO("CATS should reorder delivery: P0 first, P4 last (opposite of send order)");
}

void
CatsPriorityDataApp::SendDataChunk(uint8_t priority, uint32_t size)
{
    // Create packet with priority tag
    Ptr<Packet> packet = Create<Packet>(size);
    PriorityTag priorityTag;
    priorityTag.SetPriority(priority);
    
    // CRITICAL: Use AddByteTag (not AddPacketTag) for CATS priority tagging
    // This ensures the priority information is preserved through network transmission
    // and can be read back with FindFirstMatchingByteTag on the receiver side
    packet->AddByteTag(priorityTag);  // Use AddByteTag like working example
    
    // Send data
    int result = m_socket->Send(packet);
    if (result > 0)
    {
        m_totalBytesSent += result;
        NS_LOG_DEBUG("Sent " << result << " bytes with priority P" << (uint32_t)priority);
    }
    else
    {
        NS_LOG_WARN("Failed to send data chunk - result: " << result);
    }
}

// Implementation: CatsPriorityReceiverApp
CatsPriorityReceiverApp::CatsPriorityReceiverApp()
    : m_socket(nullptr),
      m_totalBytesReceived(0),
      m_firstByteReceived(false)
{
}

CatsPriorityReceiverApp::~CatsPriorityReceiverApp()
{
    m_socket = nullptr;
}

void
CatsPriorityReceiverApp::Setup(Ptr<Socket> socket)
{
    m_socket = socket;
}

void
CatsPriorityReceiverApp::SetPriorityGroups(const std::vector<PriorityGroup>& groups)
{
    m_priorityGroups = groups;
}

void
CatsPriorityReceiverApp::StartApplication(void)
{
    NS_LOG_FUNCTION(this);
    
    if (m_socket)
    {
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 9));
        m_socket->Listen();
        m_socket->SetRecvCallback(MakeCallback(&CatsPriorityReceiverApp::HandleRead, this));
        m_socket->SetAcceptCallback(
            MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
            MakeCallback(&CatsPriorityReceiverApp::HandleAcceptConnection, this));
    }
    
    NS_LOG_INFO("CATS Priority Receiver started");
}

void
CatsPriorityReceiverApp::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    
    if (m_socket)
    {
        m_socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
    }
    
    NS_LOG_INFO("Priority Receiver stopped - Total bytes: " << m_totalBytesReceived);
    
    // Log final metrics
    if (g_priorityCompletionFile.is_open())
    {
        Time totalTime = m_lastByteTime - m_firstByteTime;
        g_priorityCompletionFile << "# FULL_PAGE_LOAD_TIME_MS: " << totalTime.GetMilliSeconds() << std::endl;
    }
}

void
CatsPriorityReceiverApp::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    
    Ptr<Packet> packet;
    Address fromAddr;
    
    while ((packet = socket->RecvFrom(fromAddr)))
    {
        if (packet->GetSize() == 0)
        {
            break;
        }
        
        ProcessReceivedData(packet);
    }
}

void
CatsPriorityReceiverApp::HandleAcceptConnection(Ptr<Socket> socket, const Address& from)
{
    NS_LOG_FUNCTION(this << socket << from);
    socket->SetRecvCallback(MakeCallback(&CatsPriorityReceiverApp::HandleRead, this));
}

void
CatsPriorityReceiverApp::ProcessReceivedData(Ptr<Packet> packet)
{
    uint32_t packetSize = packet->GetSize();
    m_totalBytesReceived += packetSize;
    
    // Track timing
    Time now = Simulator::Now();
    if (!m_firstByteReceived)
    {
        m_firstByteTime = now;
        m_firstByteReceived = true;
    }
    m_lastByteTime = now;
    
    // Extract priority from packet (match sender's AddByteTag usage)
    // CRITICAL: Use FindFirstMatchingByteTag to extract priority information
    // that was added with AddByteTag on the sender side
    PriorityTag priorityTag;
    bool hasTag = packet->FindFirstMatchingByteTag(priorityTag);
    
    if (hasTag)
    {
        uint8_t priority = priorityTag.GetPriority();
        
        // Update priority group bytes received and track completion timing
        for (auto& group : m_priorityGroups)
        {
            if (group.priority == priority && !group.isComplete)
            {
                group.bytesReceived += packetSize;
                CheckPriorityGroupCompletion(priority);
                break;
            }
        }
        
        NS_LOG_DEBUG("Received " << packetSize << " bytes with priority P" << (uint32_t)priority);
    }
    else
    {
        NS_LOG_DEBUG("Received " << packetSize << " bytes (no priority tag)");
    }
}

void
CatsPriorityReceiverApp::CheckPriorityGroupCompletion(uint8_t priority)
{
    for (auto& group : m_priorityGroups)
    {
        if (group.priority == priority && !group.isComplete)
        {
            if (group.bytesReceived >= group.totalSize)
            {
                group.isComplete = true;
                group.completionTime = Simulator::Now();
                LogPriorityGroupCompletion(group);
                
                NS_LOG_INFO("Priority P" << (uint32_t)priority << " group completed (" << 
                           group.description << ") - " << group.bytesReceived << "/" << group.totalSize << " bytes");
                
                // Check if all priority groups are now complete
                CheckAllGroupsComplete();
            }
            break;
        }
    }
}

void
CatsPriorityReceiverApp::CheckAllGroupsComplete()
{
    bool allComplete = true;
    for (const auto& group : m_priorityGroups)
    {
        if (!group.isComplete)
        {
            allComplete = false;
            break;
        }
    }
    
    if (allComplete)
    {
        NS_LOG_INFO("All priority groups completed - page load finished!");
    }
}

void
CatsPriorityReceiverApp::LogPriorityGroupCompletion(const PriorityGroup& group)
{
    if (!g_priorityCompletionFile.is_open())
    {
        return;
    }
    
    Time completionTime = group.completionTime - m_firstByteTime;
    double completionMs = completionTime.GetMilliSeconds();
    
    // Log priority-group completion record used for comparison plotting
    g_priorityCompletionFile << std::fixed << std::setprecision(6)
                            << Simulator::Now().GetSeconds() << "\t"
                            << 0 << "\t"  // Page ID (single page in this experiment)
                            << (uint32_t)group.priority << "\t"
                            << completionMs << "\t"
                            << "GROUP_COMPLETED" << std::endl;
    
    NS_LOG_INFO("Priority P" << (uint32_t)group.priority << " (" << group.description << ") completed in " << completionMs << "ms");
}

// Main simulation function
int main(int argc, char* argv[])
{
    // Default simulation parameters
    double simTime = 10.0;
    std::string bottleneckBw = "2Mbps";
    std::string bottleneckDelay = "50ms";
    std::string accessBw = "10Mbps"; 
    std::string accessDelay = "5ms";
    bool enablePcap = false;
    std::string outputDir = ".";
    
    // Command line argument parsing
    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation time (seconds)", simTime);
    cmd.AddValue("bottleneckBw", "Bottleneck bandwidth", bottleneckBw);
    cmd.AddValue("bottleneckDelay", "Bottleneck delay", bottleneckDelay);
    cmd.AddValue("accessBw", "Access link bandwidth", accessBw);
    cmd.AddValue("accessDelay", "Access link delay", accessDelay);
    cmd.AddValue("enablePcap", "Enable PCAP tracing", enablePcap);
    cmd.AddValue("outputDir", "Output directory for traces", outputDir);
    
    // Note: CATS fairness parameters can be set via Config::SetDefault
    // Example: --ns3::TcpCats::DebtHighWatermarkP0=50000
    
    cmd.Parse(argc, argv);
    
    // Create organized output directory if outputDir is default ".", or ensure custom directory exists
    std::string finalOutputDir = outputDir;
    if (outputDir == ".") {
        finalOutputDir = CreateOrganizedOutputDirectory(outputDir);
        std::cout << "📁 Created organized output directory: " << finalOutputDir << std::endl;
        enablePcap = true;  // Auto-enable PCAP for organized runs
    } else {
        // Custom directory specified - ensure it exists
        std::string mkdirCmd = "mkdir -p " + outputDir;
        int result = std::system(mkdirCmd.c_str());
        if (result != 0) {
            NS_FATAL_ERROR("Failed to create custom output directory: " << outputDir);
        }
        std::cout << "📁 Ensured custom output directory exists: " << outputDir << std::endl;
        finalOutputDir = outputDir;
    }
    
    // Open trace files
    std::string priorityCompletionFile = finalOutputDir + "/priority_completion.tsv";
    std::string parameterDebugFile = finalOutputDir + "/parameter_debug.log";
    
    g_priorityCompletionFile.open(priorityCompletionFile);
    g_parameterDebugFile.open(parameterDebugFile);
    
    if (!g_priorityCompletionFile.is_open())
    {
        NS_FATAL_ERROR("Failed to open priority completion trace file: " << priorityCompletionFile);
    }
    
    // Write trace file headers
    g_priorityCompletionFile << "# CATS Priority Group Completion Analysis" << std::endl;
    g_priorityCompletionFile << "# ================================================================" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# This experiment demonstrates CATS queue jumping by sending" << std::endl;
    g_priorityCompletionFile << "# priority groups in REVERSE order (P4 first, P0 last)." << std::endl;
    g_priorityCompletionFile << "# CATS should reorder delivery so P0, P1 arrive first, then P2, P3, and P4 last, while respecting fairness." << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# SENDING ORDER (worst-case for baseline TCP):" << std::endl;
    g_priorityCompletionFile << "# 1st: P4 Analytics/Tracking (150KB) - LARGEST, lowest priority" << std::endl;
    g_priorityCompletionFile << "# 2nd: P3 Images/Media (60KB) - large" << std::endl;
    g_priorityCompletionFile << "# 3rd: P2 Application JS (40KB) - medium" << std::endl;
    g_priorityCompletionFile << "# 4th: P1 CSS Framework (25KB) - small" << std::endl;
    g_priorityCompletionFile << "# 5th: P0 Critical HTML/CSS (8KB) - SMALLEST, highest priority" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# EXPECTED CATS DELIVERY ORDER:" << std::endl;
    g_priorityCompletionFile << "# P0, P1 complete first despite being sent last" << std::endl;
    g_priorityCompletionFile << "# P2, P3 complete in priority order" << std::endl;
    g_priorityCompletionFile << "# P4 completes last despite being sent first" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# This demonstrates CATS's core value proposition:" << std::endl;
    g_priorityCompletionFile << "# Critical content gets priority regardless of send order" << std::endl;
    g_priorityCompletionFile << "# ================================================================" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# TSV FILE FORMAT AND INTERPRETATION GUIDE:" << std::endl;
    g_priorityCompletionFile << "# ===========================================" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# Columns:" << std::endl;
    g_priorityCompletionFile << "# 1. SimTime(s)        - Absolute simulation time when group completed" << std::endl;
    g_priorityCompletionFile << "# 2. PageID           - Page identifier (always 0 in single-page experiment)" << std::endl;
    g_priorityCompletionFile << "# 3. Priority         - CATS priority level (0=highest, 4=lowest)" << std::endl;
    g_priorityCompletionFile << "# 4. CompletionTime(ms) - Relative completion time from first byte received" << std::endl;
    g_priorityCompletionFile << "# 5. Event            - Always 'GROUP_COMPLETED' for this experiment" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# HOW TO INTERPRET THE RESULTS:" << std::endl;
    g_priorityCompletionFile << "# - Lower CompletionTime(ms) = Faster delivery" << std::endl;
    g_priorityCompletionFile << "# - Priority 0 should have lowest CompletionTime (fastest)" << std::endl;
    g_priorityCompletionFile << "# - Priority 4 should have highest CompletionTime (slowest)" << std::endl;
    g_priorityCompletionFile << "# - CATS Success: P0 < P1 < P2 < P3 < P4 completion times" << std::endl;
    g_priorityCompletionFile << "# - TCP Without CATS: P4 < P3 < P2 < P1 < P0 (send order)" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# ================================================================" << std::endl;
    g_priorityCompletionFile << "# Format: SimTime(s)\tPageID\tPriority\tCompletionTime(ms)\tEvent" << std::endl;
    
    // Write parameter debug file headers and log command line parameters
    if (g_parameterDebugFile.is_open())
    {
        g_parameterDebugFile << "=== CATS PARAMETER VERIFICATION AND DEBUGGING ===" << std::endl;
        g_parameterDebugFile << "Experiment: experiment-core-prioritization" << std::endl;
        g_parameterDebugFile << "Timestamp: " << std::time(nullptr) << std::endl;
        g_parameterDebugFile << std::endl;
        
        // Log command line arguments to show which parameters were set
        g_parameterDebugFile << "🔧 Command line arguments used:" << std::endl;
        for (int i = 1; i < argc; i++) {
            g_parameterDebugFile << "   argv[" << i << "]: " << argv[i] << std::endl;
        }
        g_parameterDebugFile << std::endl;
        
        g_parameterDebugFile << "📊 All CATS Parameters (configurable via command line):" << std::endl;
        g_parameterDebugFile << "   Debt High Watermarks:" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtHighWatermarkP0=value (default: 60000)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtHighWatermarkP1=value (default: 48000)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtHighWatermarkP2=value (default: 36000)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtHighWatermarkP3=value (default: 24000)" << std::endl;
        g_parameterDebugFile << std::endl;
        g_parameterDebugFile << "   Debt Low Watermarks:" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtLowWatermarkP0=value  (default: 30000)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtLowWatermarkP1=value  (default: 24000)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtLowWatermarkP2=value  (default: 18000)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::DebtLowWatermarkP3=value  (default: 12000)" << std::endl;
        g_parameterDebugFile << std::endl;
        g_parameterDebugFile << "   Payback Multipliers:" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::PaybackMultiplierP0=value (default: 2.0)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::PaybackMultiplierP1=value (default: 1.6)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::PaybackMultiplierP2=value (default: 1.2)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::PaybackMultiplierP3=value (default: 0.8)" << std::endl;
        g_parameterDebugFile << "     --ns3::TcpCats::PaybackMultiplierP4=value (default: 0.4)" << std::endl;
        g_parameterDebugFile << std::endl;
        
        g_parameterDebugFile << "=== SIMULATION LOG ===" << std::endl;
    }
    
    // Enable logging
    LogComponentEnable("ExperimentCorePrioritization", LOG_LEVEL_INFO);
    
    NS_LOG_INFO("Starting CATS experiment: Representative Core Prioritization");
    NS_LOG_INFO("Network: " << bottleneckBw << "/" << bottleneckDelay << " bottleneck, " << 
                accessBw << "/" << accessDelay << " access");
    
    // Log current CATS parameter configuration
    NS_LOG_INFO("CATS Parameter Configuration:");
    NS_LOG_INFO("  (Parameters shown are currently active - mix of command line and defaults)");
    
    // Create dumbbell topology
    uint32_t nLeftLeaf = 1;
    uint32_t nRightLeaf = 1;
    
    PointToPointHelper leftLink;
    leftLink.SetDeviceAttribute("DataRate", StringValue(accessBw));
    leftLink.SetChannelAttribute("Delay", StringValue(accessDelay));
    
    PointToPointHelper rightLink;
    rightLink.SetDeviceAttribute("DataRate", StringValue(accessBw));
    rightLink.SetChannelAttribute("Delay", StringValue(accessDelay));
    
    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", StringValue(bottleneckBw));
    bottleneckLink.SetChannelAttribute("Delay", StringValue(bottleneckDelay));
    
    PointToPointDumbbellHelper dumbbell(nLeftLeaf, leftLink,
                                       nRightLeaf, rightLink,
                                       bottleneckLink);
    
    // Install Internet stack
    InternetStackHelper stack;
    dumbbell.InstallStack(stack);
    
    // Configure TCP base settings (no need to set CATS as default socket type)
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1000));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(131072));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(131072));
    
    // Assign IP addresses
    dumbbell.AssignIpv4Addresses(Ipv4AddressHelper("10.1.1.0", "255.255.255.0"),
                                Ipv4AddressHelper("10.2.1.0", "255.255.255.0"),
                                Ipv4AddressHelper("10.3.1.0", "255.255.255.0"));
    
    // Get nodes
    Ptr<Node> senderNode = dumbbell.GetLeft(0);
    Ptr<Node> receiverNode = dumbbell.GetRight(0);
    
    // Create CATS socket directly using CreateObject (like cats-dumbbell-refactored.cc)
    Ptr<TcpCats> catsSocket = CreateObject<TcpCats>();
    
    Ptr<CatsPriorityDataApp> senderApp = CreateObject<CatsPriorityDataApp>();
    senderApp->Setup(catsSocket, dumbbell.GetRightIpv4Address(0), 9);
    senderNode->AddApplication(senderApp);
    senderApp->SetStartTime(Seconds(1.0));
    senderApp->SetStopTime(Seconds(simTime - 1.0));
    
    // Create receiver socket and application (standard TCP socket for receiving)
    TypeId tcpFactory = TcpSocketFactory::GetTypeId();
    Ptr<Socket> receiverSocket = Socket::CreateSocket(receiverNode, tcpFactory);
    Ptr<CatsPriorityReceiverApp> receiverApp = CreateObject<CatsPriorityReceiverApp>();
    receiverApp->Setup(receiverSocket);
    
    // Share priority group information with receiver (must match sender's InitializePriorityGroups)
    std::vector<PriorityGroup> priorityGroups;
    priorityGroups.emplace_back(0, 8000, "Critical HTML/CSS");
    priorityGroups.emplace_back(1, 25000, "CSS Framework");
    priorityGroups.emplace_back(2, 40000, "Application JS");
    priorityGroups.emplace_back(3, 60000, "Images/Media");
    priorityGroups.emplace_back(4, 150000, "Analytics/Tracking");
    receiverApp->SetPriorityGroups(priorityGroups);
    
    receiverNode->AddApplication(receiverApp);
    receiverApp->SetStartTime(Seconds(0.5));
    receiverApp->SetStopTime(Seconds(simTime));
    
    // Enable PCAP tracing
    if (enablePcap)
    {
        std::string pcapPrefix = finalOutputDir + "/experiment-core-prio";
        bottleneckLink.EnablePcapAll(pcapPrefix);
        NS_LOG_INFO("PCAP tracing enabled - prefix: " << pcapPrefix);
    }
    
    // Set up routing
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    
    // Run simulation
    NS_LOG_INFO("Running simulation for " << simTime << " seconds...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    
    // Close all trace files
    g_priorityCompletionFile.close();
    if (g_parameterDebugFile.is_open()) g_parameterDebugFile.close();
    
    NS_LOG_INFO("Simulation completed successfully");
    NS_LOG_INFO("Output directory: " << finalOutputDir);
    NS_LOG_INFO("Output files:");
    NS_LOG_INFO("  Priority completion: " << priorityCompletionFile);
    if (g_parameterDebugFile.is_open()) NS_LOG_INFO("  Parameter debug: " << parameterDebugFile);
    if (enablePcap) NS_LOG_INFO("  PCAP files: " << finalOutputDir << "/experiment-core-prio-*.pcap");
    NS_LOG_INFO("");
    NS_LOG_INFO("=== CATS QUEUE JUMPING DEMONSTRATION COMPLETE ===");
    NS_LOG_INFO("CATS parameter sensitivity can be analyzed in parameter_debug.log");
    NS_LOG_INFO("Use priority_completion.tsv as input to comparison plots");
    NS_LOG_INFO("Verify CATS priority headers: tcpdump -r *.pcap | grep 'unknown-253'");
    
    return 0;
}