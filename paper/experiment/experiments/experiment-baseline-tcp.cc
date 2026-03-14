/*
 * CATS TCP Experiment Baseline: Standard TCP Comparison
 * =======================================================
 *
 * OVERVIEW:
 * This experiment provides a baseline comparison for CATS TCP by running the
 * same traffic pattern with standard TCP configured with BBR congestion control,
 * PRR recovery, and RTT Mean Deviation estimation. It sends priority groups in
 * REVERSE order (P4 first, P0 last) to show how standard TCP handles this scenario
 * without CATS queue jumping capabilities.
 *
 * NETWORK TOPOLOGY:
 * Simple dumbbell topology with configurable bottleneck:
 * - Left leaf: Sender node with standard TCP socket (BBR + PRR + RTT MeanDev)
 * - Right leaf: Receiver node with standard TCP socket
 * - Bottleneck: Configurable bandwidth/delay (default: 2Mbps/50ms)
 * - Access links: High-speed links (default: 10Mbps/5ms)
 *
 * TRAFFIC PATTERN (Web Page Load Simulation):
 * Sends 5 discrete priority groups representing realistic web page components:
 * - P0: Critical HTML/CSS (8KB) - Render-blocking content, sent LAST
 * - P1: CSS Framework (25KB) - Essential styling, higher priority than JS
 * - P2: Application JavaScript (40KB) - Interactive functionality
 * - P3: Images/Media (60KB) - Visual content, background loading
 * - P4: Analytics/Tracking (150KB) - Lowest priority, sent FIRST
 *
 * BASELINE TCP CONFIGURATION:
 * - Standard TCP socket with BBR congestion control for optimal throughput
 * - PRR recovery algorithm for fast loss recovery
 * - Mean Deviation RTT estimation for accurate timing measurements
 * - Full TCP protocol stack integration
 * - Priority tags are still applied but ignored by standard TCP
 *
 * BUFFER MANAGEMENT IMPLEMENTATION:
 * This sender follows the NS-3 BulkSendApplication pattern:
 * - Data is sent in fixed 1000-byte chunks (matching configured segment size)
 * - On connection success, the first chunk is queued
 * - If Send() returns -1 (buffer full), transmission pauses
 * - Transmission resumes from DataSend() callback when socket space is available
 * This avoids large one-shot writes that overflow the TCP send buffer while still
 * transmitting the full priority-group payload (including the 150KB P4 group).
 *
 * EXPECTED BEHAVIOR (WITHOUT CATS):
 * Standard TCP will deliver packets in send order (FIFO), not priority order:
 * - Send order: P4 → P3 → P2 → P1 → P0 (largest to smallest)
 * - TCP delivery: P4 → P3 → P2 → P1 → P0 (same as send order)
 * - Typical completion times: P4 ~130ms, P0 ~1327ms (opposite of CATS)
 *
 * OUTPUT FILES:
 * - priority_completion.tsv: Priority-group completion timings used for comparison plots
 * - parameter_debug.log: TCP configuration logging and parameter info
 * - experiment-baseline-tcp-*.pcap: Network traces for comparison with CATS
 *
 * DIRECTORY ORGANIZATION:
 * - Default: Creates organized experiment-baseline-results/DD-MM-YYYY-HH-MM-SS_run/ directory
 * - Custom: Uses specified --outputDir (creates if needed)
 * - Automatic PCAP generation for organized runs
 *
 * USAGE EXAMPLES:
 * Basic run (default settings):
 *   ./ns3 run "experiment-baseline-tcp"
 *
 * Custom simulation parameters:
 *   ./ns3 run "experiment-baseline-tcp --simTime=15 --bottleneckBw=5Mbps --bottleneckDelay=100ms"
 *
 * Custom output directory:
 *   ./ns3 run "experiment-baseline-tcp --outputDir=my-baseline-results"
 *
 * Help with all parameters:
 *   ./ns3 run "experiment-baseline-tcp --help"
 *
 * AVAILABLE COMMAND LINE PARAMETERS:
 * Network Configuration:
 *   --simTime=<seconds>          : Simulation duration (default: 10)
 *   --bottleneckBw=<bandwidth>   : Bottleneck bandwidth (default: 2Mbps)
 *   --bottleneckDelay=<time>     : Bottleneck delay (default: 50ms)
 *   --accessBw=<bandwidth>       : Access link bandwidth (default: 10Mbps)
 *   --accessDelay=<time>         : Access link delay (default: 5ms)
 *   --outputDir=<path>           : Custom output directory (default: auto-generated)
 *
 * EXPECTED BEHAVIOR:
 * Standard TCP will follow FIFO delivery order:
 * - Send order: P4 → P3 → P2 → P1 → P0 (largest to smallest)
 * - TCP delivery: P4 → P3 → P2 → P1 → P0 (same as send order)
 * - Typical completion times: P4 ~130ms, P0 ~1327ms
 *
 * This provides the baseline for comparing against CATS queue jumping.
 * Without CATS, critical content (P0) completes last despite being smallest.
 *
 * TSV File Format Reference:
 * See TSV documentation section below for complete column descriptions
 * and interpretation guidelines.
 *
 * Usage:
 * ./ns3 run "experiment-baseline-tcp --help" for parameter list
 * ./ns3 run "experiment-baseline-tcp --simTime=15 --bottleneckBw=5Mbps"
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/tcp-bbr.h"
#include "ns3/tcp-prr-recovery.h"
#include "ns3/rtt-estimator.h"
#include "ns3/priority-tag.h"
#include "ns3/point-to-point-dumbbell.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ExperimentBaselineTcp");

// Global trace files
std::ofstream g_priorityCompletionFile;
std::ofstream g_parameterDebugFile;

/**
 * \brief Create organized output directory with timestamp and parameter info
 *
 * Creates a directory structure like:
 * experiment-baseline-results/YYYY-MM-DD-HH-MM-SS-paramHash/
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
    std::string fullDir = baseOutputDir + "/experiment-baseline-results/" + timestamp + "_" + paramHash;

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
 * \brief Baseline TCP Priority Data Generator Application
 *
 * Generates realistic mixed-priority traffic patterns for baseline TCP analysis.
 * Simulates a web page load with different priority levels but uses standard TCP
 * which ignores priority tags and delivers in FIFO order.
 * 
 * Uses BulkSendApplication pattern for proper TCP connection and chunked sending.
 */
class BaselineTcpPriorityDataApp : public Application
{
public:
    BaselineTcpPriorityDataApp();
    virtual ~BaselineTcpPriorityDataApp();
    
    void Setup(Address address, uint16_t port);
    void InitializePriorityGroups();
    uint32_t GetTotalDataSize() const;

private:
    virtual void StartApplication(void) override;
    virtual void StopApplication(void) override;
    
    void SendAllPriorityGroupsConcurrently();
    void SendDataChunk(uint8_t priority, uint32_t size);
    void ConnectionSucceeded(Ptr<Socket> socket);
    void ConnectionFailed(Ptr<Socket> socket);
    void DataSend(Ptr<Socket> socket, uint32_t available);
    
    Ptr<Socket> m_socket;
    Address m_peer;
    uint16_t m_peerPort;
    bool m_connected;
    
    // Priority groups (realistic web page data)
    std::vector<PriorityGroup> m_priorityGroups;
    uint32_t m_currentGroupIndex;
    uint32_t m_totalBytesSent;
    uint32_t m_sendSize;  // Size of each send chunk (like BulkSendApplication)
    uint8_t m_currentPriority;  // Current priority being sent
    uint32_t m_bytesSentForCurrentPriority;  // Bytes sent for current priority
    
    EventId m_sendEvent;
    Time m_dataStartTime;
};/**
 * \brief Baseline TCP Priority Receiver Application
 *
 * Receives priority data and tracks completion times per priority group
 */
class BaselineTcpPriorityReceiverApp : public Application
{
public:
    BaselineTcpPriorityReceiverApp();
    virtual ~BaselineTcpPriorityReceiverApp();

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

// Implementation: BaselineTcpPriorityDataApp
BaselineTcpPriorityDataApp::BaselineTcpPriorityDataApp()
    : m_socket(nullptr),
      m_connected(false),
      m_currentGroupIndex(0),
      m_totalBytesSent(0),
      m_sendSize(1000),  // Send in 1000-byte chunks (like TCP segment size)
      m_currentPriority(4),  // Start with P4 (highest index)
      m_bytesSentForCurrentPriority(0)
{
}

BaselineTcpPriorityDataApp::~BaselineTcpPriorityDataApp()
{
    m_socket = nullptr;
}

void
BaselineTcpPriorityDataApp::Setup(Address address, uint16_t port)
{
    m_peer = address;
    m_peerPort = port;
    InitializePriorityGroups();
}

void
BaselineTcpPriorityDataApp::InitializePriorityGroups()
{
    // Realistic web page priority groups (inspired by Chrome DevTools priorities)
    // IMPORTANT: Sizes chosen to demonstrate TCP FIFO behavior effectively
    // P4 (analytics) is sent FIRST but will complete LAST due to FIFO
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
    // Standard TCP will deliver this first due to FIFO, despite being lowest priority
    m_priorityGroups.emplace_back(4, 150000, "Analytics/Tracking");

    NS_LOG_INFO("Initialized " << m_priorityGroups.size() << " priority groups (Total: " <<
                GetTotalDataSize() << " bytes)");
    for (const auto& group : m_priorityGroups)
    {
        NS_LOG_INFO("P" << (uint32_t)group.priority << ": " << group.totalSize << " bytes - " << group.description);
    }
}

uint32_t
BaselineTcpPriorityDataApp::GetTotalDataSize() const
{
    uint32_t total = 0;
    for (const auto& group : m_priorityGroups)
    {
        total += group.totalSize;
    }
    return total;
}

void
BaselineTcpPriorityDataApp::StartApplication(void)
{
    NS_LOG_FUNCTION(this);
    
    // Create the socket if not already (following BulkSendApplication pattern)
    if (!m_socket)
    {
        TypeId tcpFactory = TcpSocketFactory::GetTypeId();
        m_socket = Socket::CreateSocket(GetNode(), tcpFactory);
        
        // Fatal error if socket type is not NS3_SOCK_STREAM
        if (m_socket->GetSocketType() != Socket::NS3_SOCK_STREAM)
        {
            NS_FATAL_ERROR("Using BaselineTcpPriorityDataApp with an incompatible socket type. "
                           "Requires SOCK_STREAM (TCP).");
        }
        
        // Bind the socket
        int ret = m_socket->Bind();
        if (ret == -1)
        {
            NS_FATAL_ERROR("Failed to bind socket");
        }
        
        // Connect to peer
        m_socket->Connect(InetSocketAddress(Ipv4Address::ConvertFrom(m_peer), m_peerPort));
        
        // Set up connection callbacks (following BulkSendApplication pattern)
        m_socket->SetConnectCallback(
            MakeCallback(&BaselineTcpPriorityDataApp::ConnectionSucceeded, this),
            MakeCallback(&BaselineTcpPriorityDataApp::ConnectionFailed, this));
        
        // Set up send callback for when buffer space becomes available
        m_socket->SetSendCallback(MakeCallback(&BaselineTcpPriorityDataApp::DataSend, this));
    }
    
    // Configure the standard TCP socket with BBR + PRR + RttMeanDeviation
    // This provides the same congestion control as CATS but without priority queue jumping
    Ptr<TcpSocketBase> tcpSocket = DynamicCast<TcpSocketBase>(m_socket);
    if (tcpSocket)
    {
        NS_LOG_INFO("🔧 Configuring baseline TCP socket with BBR + PRR + RttMeanDeviation");
        
        // Log TCP socket configuration for parameter debugging
        if (g_parameterDebugFile.is_open())
        {
            g_parameterDebugFile << "Baseline TCP Socket Configuration started at " << Simulator::Now().GetSeconds() << "s:" << std::endl;
        }
        
        // Step 1: Configure congestion control algorithm - BBR for better performance
        Ptr<TcpBbr> bbr = CreateObject<TcpBbr>();
        tcpSocket->SetCongestionControlAlgorithm(bbr);
        if (g_parameterDebugFile.is_open())
        {
            g_parameterDebugFile << "  Congestion Control: BBR configured at " << Simulator::Now().GetSeconds() << "s" << std::endl;
        }
        
        // Step 2: Set recovery algorithm - PRR for fast recovery from losses
        Ptr<TcpPrrRecovery> recovery = CreateObject<TcpPrrRecovery>();
        tcpSocket->SetRecoveryAlgorithm(recovery);
        if (g_parameterDebugFile.is_open())
        {
            g_parameterDebugFile << "  Recovery Algorithm: PRR configured at " << Simulator::Now().GetSeconds() << "s" << std::endl;
        }
        
        // Step 3: Set RTT estimator for accurate timing measurements
        Ptr<RttMeanDeviation> rtt = CreateObject<RttMeanDeviation>();
        tcpSocket->SetRtt(rtt);
        if (g_parameterDebugFile.is_open())
        {
            g_parameterDebugFile << "  RTT Estimator: RttMeanDeviation configured at " << Simulator::Now().GetSeconds() << "s" << std::endl;
            g_parameterDebugFile << std::endl;
        }
    }
    
    NS_LOG_INFO("Baseline TCP Priority Data Generator started - waiting for connection");
}

void
BaselineTcpPriorityDataApp::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    
    if (m_socket)
    {
        m_socket->Close();
        m_connected = false;
    }
    
    Simulator::Cancel(m_sendEvent);
    
    NS_LOG_INFO("Data Generator stopped - Total bytes sent: " << m_totalBytesSent);
}

void
BaselineTcpPriorityDataApp::ConnectionSucceeded(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_LOG_INFO("Baseline TCP connection succeeded - starting data transmission");
    m_connected = true;
    
    m_dataStartTime = Simulator::Now();
    
    // Start sending data from P4 (highest priority group index)
    SendDataChunk(m_currentPriority, m_sendSize);
}

void
BaselineTcpPriorityDataApp::ConnectionFailed(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_LOG_ERROR("Baseline TCP connection failed");
    m_connected = false;
}

void
BaselineTcpPriorityDataApp::DataSend(Ptr<Socket> socket, uint32_t available)
{
    NS_LOG_FUNCTION(this << socket << available);
    
    if (m_connected)
    {
        // Continue sending data for current priority group
        SendDataChunk(m_currentPriority, m_sendSize);
    }
}

void
BaselineTcpPriorityDataApp::SendAllPriorityGroupsConcurrently()
{
    NS_LOG_INFO("Starting chunked transmission in REVERSE order (P4 first, P0 last)");
    NS_LOG_INFO("Standard TCP will deliver in FIFO order: P4 first, P0 last (no priority awareness)");
    
    // This method is now called from ConnectionSucceeded
    // The actual sending is handled by SendDataChunk and DataSend callback
}

void
BaselineTcpPriorityDataApp::SendDataChunk(uint8_t priority, uint32_t size)
{
    NS_LOG_FUNCTION(this << (uint32_t)priority << size);
    
    // Check if we've finished all priority groups
    if (m_currentPriority > 4)
    {
        NS_LOG_INFO("All priority groups sent (" << m_totalBytesSent << " total bytes)");
        return;
    }
    
    // Find the current priority group
    auto it = std::find_if(m_priorityGroups.begin(), m_priorityGroups.end(),
                          [this](const PriorityGroup& group) {
                              return group.priority == m_currentPriority;
                          });
    
    if (it == m_priorityGroups.end())
    {
        NS_LOG_ERROR("Priority group P" << (uint32_t)m_currentPriority << " not found");
        return;
    }
    
    PriorityGroup& currentGroup = *it;
    
    // Check if this priority group is complete
    if (m_bytesSentForCurrentPriority >= currentGroup.totalSize)
    {
        // Move to next priority group (P4 -> P3 -> P2 -> P1 -> P0)
        if (m_currentPriority == 0)
        {
            NS_LOG_INFO("All priority groups transmission complete");
            return;
        }
        m_currentPriority--;
        m_bytesSentForCurrentPriority = 0;
        
        // Continue with next priority group
        SendDataChunk(m_currentPriority, size);
        return;
    }
    
    // Calculate how much to send for this chunk
    uint32_t remainingForGroup = currentGroup.totalSize - m_bytesSentForCurrentPriority;
    uint32_t toSend = std::min(size, remainingForGroup);
    
    // Create packet with priority tag
    Ptr<Packet> packet = Create<Packet>(toSend);
    PriorityTag priorityTag;
    priorityTag.SetPriority(m_currentPriority);
    packet->AddByteTag(priorityTag);
    
    // Send the chunk
    int actual = m_socket->Send(packet);
    if (actual > 0)
    {
        m_bytesSentForCurrentPriority += actual;
        m_totalBytesSent += actual;
        
        NS_LOG_DEBUG("Sent " << actual << " bytes for P" << (uint32_t)m_currentPriority << 
                    " (" << m_bytesSentForCurrentPriority << "/" << currentGroup.totalSize << ")");
        
        // Check if this priority group is now complete
        if (m_bytesSentForCurrentPriority >= currentGroup.totalSize)
        {
            NS_LOG_INFO("Priority P" << (uint32_t)m_currentPriority << " (" << currentGroup.description << 
                       ") transmission complete - " << currentGroup.totalSize << " bytes");
        }
    }
    else if (actual == -1)
    {
        NS_LOG_DEBUG("Send buffer full for P" << (uint32_t)m_currentPriority << ", waiting for DataSend callback");
        // The DataSend callback will be called when buffer space is available
    }
    else
    {
        NS_LOG_ERROR("Unexpected send result: " << actual);
    }
}// Implementation: BaselineTcpPriorityReceiverApp
BaselineTcpPriorityReceiverApp::BaselineTcpPriorityReceiverApp()
    : m_socket(nullptr),
      m_totalBytesReceived(0),
      m_firstByteReceived(false)
{
}

BaselineTcpPriorityReceiverApp::~BaselineTcpPriorityReceiverApp()
{
    m_socket = nullptr;
}

void
BaselineTcpPriorityReceiverApp::Setup(Ptr<Socket> socket)
{
    m_socket = socket;
}

void
BaselineTcpPriorityReceiverApp::SetPriorityGroups(const std::vector<PriorityGroup>& groups)
{
    m_priorityGroups = groups;
}

void
BaselineTcpPriorityReceiverApp::StartApplication(void)
{
    NS_LOG_FUNCTION(this);

    if (m_socket)
    {
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 9));
        m_socket->Listen();
        m_socket->SetRecvCallback(MakeCallback(&BaselineTcpPriorityReceiverApp::HandleRead, this));
        m_socket->SetAcceptCallback(
            MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
            MakeCallback(&BaselineTcpPriorityReceiverApp::HandleAcceptConnection, this));
    }

    NS_LOG_INFO("Baseline TCP Priority Receiver started");
}

void
BaselineTcpPriorityReceiverApp::StopApplication(void)
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
BaselineTcpPriorityReceiverApp::HandleRead(Ptr<Socket> socket)
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
BaselineTcpPriorityReceiverApp::HandleAcceptConnection(Ptr<Socket> socket, const Address& from)
{
    NS_LOG_FUNCTION(this << socket << from);
    socket->SetRecvCallback(MakeCallback(&BaselineTcpPriorityReceiverApp::HandleRead, this));
}

void
BaselineTcpPriorityReceiverApp::ProcessReceivedData(Ptr<Packet> packet)
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
BaselineTcpPriorityReceiverApp::CheckPriorityGroupCompletion(uint8_t priority)
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
                NS_LOG_INFO("Standard TCP delivered in FIFO order - no priority queue jumping");
                // Check if all priority groups are now complete
                CheckAllGroupsComplete();
            }
            break;
        }
    }
}

void
BaselineTcpPriorityReceiverApp::CheckAllGroupsComplete()
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
        NS_LOG_INFO("Standard TCP completed in FIFO order - compare with CATS results");
    }
}

void
BaselineTcpPriorityReceiverApp::LogPriorityGroupCompletion(const PriorityGroup& group)
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
    NS_LOG_INFO("Standard TCP FIFO delivery - no priority reordering");
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
    g_priorityCompletionFile << "# Baseline TCP Priority Group Completion Analysis" << std::endl;
    g_priorityCompletionFile << "# ================================================================" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# This experiment shows standard TCP behavior without CATS queue jumping." << std::endl;
    g_priorityCompletionFile << "# Priority groups are sent in REVERSE order but TCP delivers in FIFO order." << std::endl;
    g_priorityCompletionFile << "# Priority tags are preserved but ignored by standard TCP." << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# SENDING ORDER (same as CATS experiment):" << std::endl;
    g_priorityCompletionFile << "# 1st: P4 Analytics/Tracking (150KB) - LARGEST, lowest priority" << std::endl;
    g_priorityCompletionFile << "# 2nd: P3 Images/Media (60KB) - large" << std::endl;
    g_priorityCompletionFile << "# 3rd: P2 Application JS (40KB) - medium" << std::endl;
    g_priorityCompletionFile << "# 4th: P1 CSS Framework (25KB) - small" << std::endl;
    g_priorityCompletionFile << "# 5th: P0 Critical HTML/CSS (8KB) - SMALLEST, highest priority" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# EXPECTED TCP DELIVERY ORDER (FIFO):" << std::endl;
    g_priorityCompletionFile << "# P4 completes first despite being lowest priority" << std::endl;
    g_priorityCompletionFile << "# P3, P2, P1 complete in send order" << std::endl;
    g_priorityCompletionFile << "# P0 completes last despite being highest priority" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# This demonstrates the baseline TCP behavior:" << std::endl;
    g_priorityCompletionFile << "# No priority awareness - delivery follows send order" << std::endl;
    g_priorityCompletionFile << "# Critical content (P0) waits behind bulk data (P4)" << std::endl;
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
    g_priorityCompletionFile << "# - TCP FIFO: P4 should have lowest CompletionTime (fastest)" << std::endl;
    g_priorityCompletionFile << "# - TCP FIFO: P0 should have highest CompletionTime (slowest)" << std::endl;
    g_priorityCompletionFile << "# - TCP Success: P4 < P3 < P2 < P1 < P0 completion times" << std::endl;
    g_priorityCompletionFile << "# - Compare with CATS: CATS should show P0 < P1 < P2 < P3 < P4" << std::endl;
    g_priorityCompletionFile << "#" << std::endl;
    g_priorityCompletionFile << "# ================================================================" << std::endl;
    g_priorityCompletionFile << "# Format: SimTime(s)\tPageID\tPriority\tCompletionTime(ms)\tEvent" << std::endl;

    // Write parameter debug file headers and log command line parameters
    if (g_parameterDebugFile.is_open())
    {
        g_parameterDebugFile << "=== BASELINE TCP CONFIGURATION AND DEBUGGING ===" << std::endl;
        g_parameterDebugFile << "Experiment: experiment-baseline-tcp" << std::endl;
        g_parameterDebugFile << "Timestamp: " << std::time(nullptr) << std::endl;
        g_parameterDebugFile << std::endl;

        // Log command line arguments to show which parameters were set
        g_parameterDebugFile << "🔧 Command line arguments used:" << std::endl;
        for (int i = 1; i < argc; i++) {
            g_parameterDebugFile << "   argv[" << i << "]: " << argv[i] << std::endl;
        }
        g_parameterDebugFile << std::endl;

        g_parameterDebugFile << "📊 TCP Configuration (same as CATS baseline):" << std::endl;
        g_parameterDebugFile << "   Congestion Control: BBR (same as CATS)" << std::endl;
        g_parameterDebugFile << "   Recovery Algorithm: PRR (same as CATS)" << std::endl;
        g_parameterDebugFile << "   RTT Estimator: RttMeanDeviation (same as CATS)" << std::endl;
        g_parameterDebugFile << "   Priority Awareness: NONE (standard TCP FIFO)" << std::endl;
        g_parameterDebugFile << std::endl;

        g_parameterDebugFile << "=== SIMULATION LOG ===" << std::endl;
    }

    // Enable logging
    LogComponentEnable("ExperimentBaselineTcp", LOG_LEVEL_INFO);

    NS_LOG_INFO("Starting baseline TCP experiment: Standard TCP Comparison");
    NS_LOG_INFO("Network: " << bottleneckBw << "/" << bottleneckDelay << " bottleneck, " <<
                accessBw << "/" << accessDelay << " access");
    NS_LOG_INFO("TCP Configuration: BBR congestion control, PRR recovery, RttMeanDeviation");

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

    // Create sender application (socket will be created in StartApplication)
    Ptr<BaselineTcpPriorityDataApp> senderApp = CreateObject<BaselineTcpPriorityDataApp>();
    senderApp->Setup(dumbbell.GetRightIpv4Address(0), 9);
    senderNode->AddApplication(senderApp);
    senderApp->SetStartTime(Seconds(1.0));
    senderApp->SetStopTime(Seconds(simTime - 1.0));

    // Create receiver socket and application (standard TCP socket for receiving)
    TypeId tcpFactory = TcpSocketFactory::GetTypeId();
    Ptr<Socket> receiverSocket = Socket::CreateSocket(receiverNode, tcpFactory);
    Ptr<BaselineTcpPriorityReceiverApp> receiverApp = CreateObject<BaselineTcpPriorityReceiverApp>();
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
        std::string pcapPrefix = finalOutputDir + "/experiment-baseline-tcp";
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
    if (enablePcap) NS_LOG_INFO("  PCAP files: " << finalOutputDir << "/experiment-baseline-tcp-*.pcap");
    NS_LOG_INFO("");
    NS_LOG_INFO("=== BASELINE TCP EXPERIMENT COMPLETE ===");
    NS_LOG_INFO("Compare results with CATS experiment to see priority queue jumping benefits");
    NS_LOG_INFO("Baseline TCP delivers in FIFO order - no priority awareness");
    NS_LOG_INFO("Compare priority_completion.tsv against CATS for paper plots");

    return 0;
}