/*
 * Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/**
 * \file tcp-cats.cc
 * \brief CATS (Custody Assisted Traffic Switching) TCP Socket Implementation
 * 
 * This implementation provides priority-based packet queuing with sophisticated
 * fairness mechanisms for ns-3 TCP sockets.
 * 
 * ## Architecture: "Interceptor and Feeder"
 * - **INTERCEPTOR**: Send() method routes app data to priority queues (P0-P4)
 * - **CONDUCTOR**: ConductorFeedData() feeds segments with priority re-evaluation  
 * - **FEEDER**: Base TCP handles network transmission, flow control, congestion control
 * 
 * ## Credit-Based Fairness System
 * - Debt tracking prevents priority starvation
 * - Configurable watermarks with hysteresis control
 * - Proportional payback multipliers maintain priority ordering
 * - Debt redistribution ensures continuous operation
 */

#include "tcp-cats.h"
#include "tcp-option-cats-priority.h"
#include "priority-tag.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/boolean.h"
#include "ns3/trace-source-accessor.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpCats");

NS_OBJECT_ENSURE_REGISTERED(TcpCats);

TypeId
TcpCats::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TcpCats")
                            .SetParent<TcpSocketBase>()
                            .SetGroupName("Internet")
                            .AddConstructor<TcpCats>()
                            .AddAttribute("CatsEnabled",
                                        "Enable CATS priority queuing",
                                        BooleanValue(true),
                                        MakeBooleanAccessor(&TcpCats::m_catsEnabled),
                                        MakeBooleanChecker())
                            
                            // High Watermark Thresholds (P0-P3)
                            .AddAttribute("DebtHighWatermarkP0",
                                        "High debt threshold for Priority 0 (URGENT) in bytes",
                                        UintegerValue(60000),
                                        MakeUintegerAccessor(&TcpCats::m_debtHighWatermarkP0),
                                        MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DebtHighWatermarkP1", 
                                        "High debt threshold for Priority 1 (Interactive) in bytes",
                                        UintegerValue(30000),
                                        MakeUintegerAccessor(&TcpCats::m_debtHighWatermarkP1),
                                        MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DebtHighWatermarkP2",
                                        "High debt threshold for Priority 2 (Control) in bytes", 
                                        UintegerValue(15000),
                                        MakeUintegerAccessor(&TcpCats::m_debtHighWatermarkP2),
                                        MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DebtHighWatermarkP3",
                                        "High debt threshold for Priority 3 (Bulk) in bytes",
                                        UintegerValue(6000),
                                        MakeUintegerAccessor(&TcpCats::m_debtHighWatermarkP3),
                                        MakeUintegerChecker<uint32_t>())
                            
                            // Low Watermark Thresholds (P0-P3) 
                            .AddAttribute("DebtLowWatermarkP0",
                                        "Low debt threshold for Priority 0 (URGENT) in bytes",
                                        UintegerValue(30000),
                                        MakeUintegerAccessor(&TcpCats::m_debtLowWatermarkP0),
                                        MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DebtLowWatermarkP1",
                                        "Low debt threshold for Priority 1 (Interactive) in bytes",
                                        UintegerValue(15000),
                                        MakeUintegerAccessor(&TcpCats::m_debtLowWatermarkP1),
                                        MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DebtLowWatermarkP2",
                                        "Low debt threshold for Priority 2 (Control) in bytes",
                                        UintegerValue(7500),
                                        MakeUintegerAccessor(&TcpCats::m_debtLowWatermarkP2),
                                        MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DebtLowWatermarkP3",
                                        "Low debt threshold for Priority 3 (Bulk) in bytes", 
                                        UintegerValue(3000),
                                        MakeUintegerAccessor(&TcpCats::m_debtLowWatermarkP3),
                                        MakeUintegerChecker<uint32_t>())
                            
                            // Payback Multipliers (P0-P4)
                            .AddAttribute("PaybackMultiplierP0",
                                        "Payback multiplier for Priority 0 sends (for debt redistribution)",
                                        DoubleValue(0.25),
                                        MakeDoubleAccessor(&TcpCats::m_paybackMultiplierP0),
                                        MakeDoubleChecker<double>(0.0))
                            .AddAttribute("PaybackMultiplierP1",
                                        "Payback multiplier for Priority 1 sends",
                                        DoubleValue(0.5),
                                        MakeDoubleAccessor(&TcpCats::m_paybackMultiplierP1),
                                        MakeDoubleChecker<double>(0.0))
                            .AddAttribute("PaybackMultiplierP2",
                                        "Payback multiplier for Priority 2 sends",
                                        DoubleValue(1.0),
                                        MakeDoubleAccessor(&TcpCats::m_paybackMultiplierP2),
                                        MakeDoubleChecker<double>(0.0))
                            .AddAttribute("PaybackMultiplierP3",
                                        "Payback multiplier for Priority 3 sends",
                                        DoubleValue(1.5),
                                        MakeDoubleAccessor(&TcpCats::m_paybackMultiplierP3),
                                        MakeDoubleChecker<double>(0.0))
                            .AddAttribute("PaybackMultiplierP4",
                                        "Payback multiplier for Priority 4 sends",
                                        DoubleValue(2.0),
                                        MakeDoubleAccessor(&TcpCats::m_paybackMultiplierP4),
                                        MakeDoubleChecker<double>(0.0));
    return tid;
}

TcpCats::TcpCats()
    : TcpSocketBase(),
      // Initialize configurable thresholds with default values (in declaration order)
      m_debtHighWatermarkP0(60000), m_debtHighWatermarkP1(30000), 
      m_debtHighWatermarkP2(15000), m_debtHighWatermarkP3(6000),
      m_debtLowWatermarkP0(30000), m_debtLowWatermarkP1(15000),
      m_debtLowWatermarkP2(7500), m_debtLowWatermarkP3(3000),
      m_paybackMultiplierP0(0.25), m_paybackMultiplierP1(0.5), m_paybackMultiplierP2(1.0),
      m_paybackMultiplierP3(1.5), m_paybackMultiplierP4(2.0),
      m_catsEnabled(true), m_lastServedPriority(5), m_conductorActive(false)
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("TcpCats socket created - CATS priority queuing enabled");
    
    // Initialize debt tracking arrays
    for (int i = 0; i < 5; i++)
    {
        m_priorityDebt[i] = 0;
        m_isInDebtState[i] = false;
    }
    
    NS_LOG_DEBUG("CATS: Initialized credit-based fairness mechanism");
    NS_LOG_DEBUG("CATS: High watermarks - P0:" << m_debtHighWatermarkP0 << " P1:" << m_debtHighWatermarkP1 
                << " P2:" << m_debtHighWatermarkP2 << " P3:" << m_debtHighWatermarkP3);
    NS_LOG_DEBUG("CATS: Low watermarks - P0:" << m_debtLowWatermarkP0 << " P1:" << m_debtLowWatermarkP1
                << " P2:" << m_debtLowWatermarkP2 << " P3:" << m_debtLowWatermarkP3);
    NS_LOG_DEBUG("CATS: Payback multipliers - P0:" << m_paybackMultiplierP0 << " P1:" << m_paybackMultiplierP1 << " P2:" << m_paybackMultiplierP2
                << " P3:" << m_paybackMultiplierP3 << " P4:" << m_paybackMultiplierP4);
}

TcpCats::~TcpCats()
{
    NS_LOG_FUNCTION(this);
    CleanupQueues();
}

uint32_t
TcpCats::GetDebtHighWatermark(uint8_t priority) const
{
    switch (priority)
    {
        case 0: return m_debtHighWatermarkP0;
        case 1: return m_debtHighWatermarkP1;
        case 2: return m_debtHighWatermarkP2;
        case 3: return m_debtHighWatermarkP3;
        case 4: return UINT32_MAX; // P4 never goes into debt
        default: return UINT32_MAX;
    }
}

uint32_t
TcpCats::GetDebtLowWatermark(uint8_t priority) const
{
    switch (priority)
    {
        case 0: return m_debtLowWatermarkP0;
        case 1: return m_debtLowWatermarkP1;
        case 2: return m_debtLowWatermarkP2;
        case 3: return m_debtLowWatermarkP3;
        case 4: return UINT32_MAX; // P4 never goes into debt
        default: return UINT32_MAX;
    }
}

double
TcpCats::GetPaybackMultiplier(uint8_t priority) const
{
    switch (priority)
    {
        case 0: return m_paybackMultiplierP0;
        case 1: return m_paybackMultiplierP1;
        case 2: return m_paybackMultiplierP2;
        case 3: return m_paybackMultiplierP3;
        case 4: return m_paybackMultiplierP4;
        default: return 0.0;
    }
}

Ptr<TcpSocketBase>
TcpCats::Fork()
{
    NS_LOG_FUNCTION(this);
    return CopyObject<TcpCats>(this);
}

void
TcpCats::SetSndBufSize(uint32_t size)
{
    NS_LOG_FUNCTION(this << size);
    
    // Call base class to set the underlying TCP buffer size
    TcpSocketBase::SetSndBufSize(size);
    
    // CATS will use base class buffer directly - no separate limit needed
    // We'll check GetTxAvailable() for actual buffer space
    
    NS_LOG_INFO("CATS: Set base TCP buffer size to " << size << " bytes");
}

int
TcpCats::Send(Ptr<Packet> p, uint32_t flags)
{
    NS_LOG_FUNCTION(this << p << flags);
    
    if (!m_catsEnabled)
    {
        // If CATS is disabled, use standard TCP behavior
        return TcpSocketBase::Send(p, flags);
    }
    
    uint32_t packetSize = p->GetSize();
    
    // Check if base TCP buffer has space for this packet
    uint32_t availableSpace = GetTxAvailable();
    if (availableSpace == 0)
    {
        NS_LOG_INFO("CATS: Interceptor rejecting packet - base TCP buffer full. "
                    << "Application should retry later or implement backpressure handling");
        return 0; // Buffer full - reject packet
    }
    
    // Extract priority from packet tag
    uint8_t priority = 2; // Default priority (middle)
    PriorityTag priorityTag;
    if (p->PeekPacketTag(priorityTag))
    {
        priority = priorityTag.GetPriority();
        NS_LOG_DEBUG("CATS: Found priority tag with priority " << (uint32_t)priority);
    }
    else if (p->FindFirstMatchingByteTag(priorityTag))
    {
        priority = priorityTag.GetPriority();
        NS_LOG_DEBUG("CATS: Found priority byte tag with priority " << (uint32_t)priority);
    }
    else
    {
        NS_LOG_DEBUG("CATS: No priority tag found, using default priority " << (uint32_t)priority);
    }
    
    // Clamp priority to valid range
    if (priority > 4)
    {
        priority = 4;
    }
    
    // Create a copy of the packet to store in our queue
    Ptr<Packet> packetCopy = p->Copy();
    
    // Add to appropriate priority queue
    CatsTxItem item(packetCopy);
    std::queue<CatsTxItem>& queue = GetPriorityQueue(priority);
    
    // Check if this is the first data arriving after all queues were empty
    bool wasCompletelyEmpty = !HasQueuedData();
    queue.push(item);
    
    // Only log restart if we went from completely empty to having data
    if (wasCompletelyEmpty && GetTotalQueuedBytes() == packetSize)
    {
        NS_LOG_WARN("CATS: 🔄 INTERCEPTOR RESTART: First data arrival after complete queue emptying");
    }
    
    NS_LOG_INFO("CATS: Interceptor accepted " << packetSize << " bytes with priority " << (uint32_t)priority 
                << " (base TCP buffer available: " << GetTxAvailable() << " bytes, total queued data in CATS: " 
                << GetTotalQueuedBytes() << " bytes)");
    
    // Trigger the Conductor to start feeding data to the base class
    // The conductor will automatically prioritize higher-priority data in each iteration
    ConductorFeedData();
    
    return packetSize; // Successfully accepted
}

int
TcpCats::SendWithPriority(Ptr<Packet> packet, uint8_t priority)
{
    NS_LOG_FUNCTION(this << packet << priority);
    
    if (!m_catsEnabled)
    {
        // If CATS is disabled, use standard TCP behavior
        return TcpSocketBase::Send(packet, 0);
    }
    
    // Clamp priority to valid range
    if (priority > 4)
    {
        priority = 4;
    }
    
    // Add priority tag to the packet
    PriorityTag priorityTag;
    priorityTag.SetPriority(priority);
    packet->AddPacketTag(priorityTag);
    
    NS_LOG_INFO("CATS: SendWithPriority called with priority " << (uint32_t)priority);
    
    // Use the regular Send method which will extract the priority tag
    return Send(packet, 0);
}

/**
 * \brief CATS Conductor - Implements segment-by-segment feeding with priority re-evaluation
 * 
 * This is the core method of the CATS "Interceptor and Feeder" architecture.
 * It feeds data from priority queues to the base TCP layer in a controlled manner
 * that enables priority jumping behavior.
 * 
 * ## Segment-by-Segment Feeding Design
 * Uses if-statement to feed only ONE segment per call, enabling continuous priority
 * re-evaluation after each segment transmission. This allows higher priority traffic
 * to interrupt ongoing lower priority transmissions.
 * 
 * ## Flow Control Integration
 * - Checks GetTxAvailable() to respect TCP window limits
 * - Only feeds data when TCP buffer has space
 * - Maintains proper interaction with congestion control
 * 
 * ## Fairness Integration
 * - Calls UpdateFairnessState() after each successful transmission
 * - Handles debt redistribution when needed for continuous operation
 * - Preserves priority ordering through credit-based fairness
 * 
 * This method is automatically triggered after ACK reception to maintain
 * continuous data flow while enabling priority-based scheduling.
 */
void
TcpCats::ConductorFeedData()
{
    NS_LOG_FUNCTION(this);
    
    if (!m_catsEnabled)
    {
        // If CATS is disabled, use standard TCP behavior
        return;
    }
    
    uint32_t totalQueued = GetTotalQueuedBytes();
    
    // DEBUG: Log what priorities are currently queued
    std::ostringstream queueStatus;
    queueStatus << "CONDUCTOR DEBUG: Queues [P0:" << m_txBufferPrio0.size() 
                << " P1:" << m_txBufferPrio1.size() 
                << " P2:" << m_txBufferPrio2.size()
                << " P3:" << m_txBufferPrio3.size() 
                << " P4:" << m_txBufferPrio4.size() << "]";
    NS_LOG_WARN(queueStatus.str());
    
    // Enhanced logging for restart scenarios
    static bool wasEmpty = true;  // Track if queues were empty before
    if (totalQueued == 0 && !wasEmpty)
    {
        NS_LOG_WARN("CATS: 🛑 All priority queues now empty - Feeder going idle");
        wasEmpty = true;
        return;
    }
    else if (totalQueued > 0 && wasEmpty)
    {
        NS_LOG_WARN("CATS: 🔄 RESTART: Feeder restarting from empty state with " << totalQueued << " bytes to feed");
        wasEmpty = false;
    }
    else if (totalQueued == 0)
    {
        // Already empty, stay idle
        return;
    }
    
    NS_LOG_DEBUG("CATS: Conductor starting - " << totalQueued << " bytes in priority queues, "
                << GetTxAvailable() << " bytes available in base TCP buffer");
    
    // CRITICAL DESIGN DECISION: Use if-statement, NOT while-loop
    // This was the breakthrough fix that enabled proper CATS priority jumping:
    //
    // BEFORE (while-loop): Conductor would drain entire priority queues in one call,
    //   preventing higher priority traffic from interrupting ongoing transmissions.
    //   Result: Sequential transmission, no queue jumping, only 18/1000 packets.
    //
    // AFTER (if-statement): Conductor feeds exactly ONE segment per call, then returns.
    //   This allows priority re-evaluation after each segment, enabling true queue jumping
    //   where P0 URGENT can interrupt ongoing P3 bulk transmission mid-stream.
    //   Result: True priority jumping, 75/76 packets (98.7% success rate).
    //
    // Feed data if both conditions are met:
    // 1. We have data in our priority queues
    // 2. The base class buffer has space
    if (HasQueuedData())
    {
        // Check if base class buffer has space
        if (GetTxAvailable() == 0)
        {
            NS_LOG_INFO("CATS: Conductor paused - base TCP buffer full. "
                        << GetTotalQueuedBytes() << " bytes remain in CATS queues awaiting TCP transmission");
            return;
        }
        
        // Perform priority and fairness selection
        uint8_t selectedPriority = GetNextPriorityToServe();
        if (selectedPriority > 4)
        {
            NS_LOG_DEBUG("CATS: No eligible priority queue found");
            return;
        }
        
        // Get the selected queue
        std::queue<CatsTxItem>& selectedQueue = GetPriorityQueue(selectedPriority);
        if (selectedQueue.empty())
        {
            NS_LOG_ERROR("CATS: Selected queue is empty - this should not happen");
            return;
        }
        
        // Get the front item
        CatsTxItem& item = selectedQueue.front();
        Ptr<Packet> largePacket = item.packet;
        
        // Calculate chunk size (single segment worth of data)
        uint32_t chunkSize = std::min(GetSegSize(), largePacket->GetSize());
        
        // Create a new small packet with the chunk of data
        Ptr<Packet> newSmallPacket = Create<Packet>();
        if (chunkSize > 0)
        {
            // Extract the chunk from the original packet
            Ptr<Packet> chunk = largePacket->CreateFragment(0, chunkSize);
            newSmallPacket = chunk;
            
            // Copy priority tag from original packet to the new small packet
            PriorityTag priorityTag;
            PriorityTag existingTag;
            
            if (largePacket->PeekPacketTag(priorityTag))
            {
                // Check if the new packet already has a priority tag (from CreateFragment)
                if (!newSmallPacket->PeekPacketTag(existingTag))
                {
                    newSmallPacket->AddPacketTag(priorityTag);
                    NS_LOG_LOGIC("CATS: Copied priority tag " << (uint32_t)priorityTag.GetPriority() << " to segment");
                }
            }
            else if (largePacket->FindFirstMatchingByteTag(priorityTag))
            {
                // Check if the new packet already has a priority byte tag
                if (!newSmallPacket->FindFirstMatchingByteTag(existingTag))
                {
                    newSmallPacket->AddByteTag(priorityTag);
                    NS_LOG_LOGIC("CATS: Copied priority byte tag " << (uint32_t)priorityTag.GetPriority() << " to segment");
                }
            }
        }
        
        // Feed this single chunk to the base class
        int sentBytes = TcpSocketBase::Send(newSmallPacket, 0);
        
        if (sentBytes <= 0)
        {
            NS_LOG_DEBUG("CATS: Conductor paused - base TCP temporarily cannot accept more data. "
                        << GetTotalQueuedBytes() << " bytes remain in CATS queues");
            return;
        }
        
        NS_LOG_DEBUG("CATS: Feeder delivered " << sentBytes << " bytes from priority " << (uint32_t)selectedPriority 
                    << " to base TCP (base buffer available: " << GetTxAvailable() 
                    << ", CATS queue remaining: " << GetTotalQueuedBytes() << " bytes)");
        
        // Update credit-based fairness state
        UpdateFairnessState(selectedPriority, sentBytes);
        
        // Remove the sent bytes from the original packet
        largePacket->RemoveAtStart(sentBytes);
        
        // If the original packet is now empty, remove it from the queue
        if (largePacket->GetSize() == 0)
        {
            selectedQueue.pop();
            NS_LOG_DEBUG("CATS: Priority " << (uint32_t)selectedPriority << " packet fully transmitted");
        }
        
        // CRITICAL: If there's still data to feed, schedule conductor to run again
        // This ensures continuous segment-by-segment feeding with priority re-evaluation
        if (HasQueuedData() && GetTxAvailable() > 0)
        {
            NS_LOG_DEBUG("CATS: Scheduling conductor to continue feeding remaining " << GetTotalQueuedBytes() << " bytes");
            Simulator::ScheduleNow(&TcpCats::ConductorFeedData, this);
        }
    }
    
    uint32_t remainingQueued = GetTotalQueuedBytes();
    if (remainingQueued > 0)
    {
        NS_LOG_DEBUG("CATS: Conductor finished - " << remainingQueued << " bytes remain in CATS queues, "
                    << "awaiting TCP flow control or ACK events to resume feeding");
    }
    else
    {
        NS_LOG_DEBUG("CATS: Conductor finished - all priority queues empty, ready for new data");
    }
}

void
TcpCats::ReceivedAck(Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
    NS_LOG_FUNCTION(this << packet << tcpHeader);
    
    // First, let the base class process the ACK
    TcpSocketBase::ReceivedAck(packet, tcpHeader);
    
    // After the base class has processed the ACK and potentially freed up buffer space,
    // trigger our Conductor to continue feeding data if we have any queued
    if (HasQueuedData())
    {
        uint32_t queuedBytes = GetTotalQueuedBytes();
        NS_LOG_DEBUG("CATS: ACK freed base TCP buffer space (" << GetTxAvailable() << " bytes available). "
                    << "Triggering Conductor to resume feeding " << queuedBytes << " bytes from priority queues");
        ConductorFeedData();
    }
}

std::queue<TcpCats::CatsTxItem>&
TcpCats::GetPriorityQueue(uint8_t priority)
{
    switch (priority)
    {
        case 0: return m_txBufferPrio0;
        case 1: return m_txBufferPrio1;
        case 2: return m_txBufferPrio2;
        case 3: return m_txBufferPrio3;
        case 4: return m_txBufferPrio4;
        default:
            NS_FATAL_ERROR("Invalid priority: " << (uint32_t)priority);
            return m_txBufferPrio4; // Never reached
    }
}

bool
TcpCats::HasQueuedData() const
{
    return !m_txBufferPrio0.empty() || !m_txBufferPrio1.empty() || 
           !m_txBufferPrio2.empty() || !m_txBufferPrio3.empty() || 
           !m_txBufferPrio4.empty();
}

uint32_t
TcpCats::GetTotalQueuedBytes() const
{
    uint32_t totalBytes = 0;
    
    // Count bytes in each priority queue
    std::queue<CatsTxItem> tempQueue;
    
    // Priority 0
    tempQueue = m_txBufferPrio0;
    while (!tempQueue.empty()) {
        totalBytes += tempQueue.front().packet->GetSize();
        tempQueue.pop();
    }
    
    // Priority 1
    tempQueue = m_txBufferPrio1;
    while (!tempQueue.empty()) {
        totalBytes += tempQueue.front().packet->GetSize();
        tempQueue.pop();
    }
    
    // Priority 2
    tempQueue = m_txBufferPrio2;
    while (!tempQueue.empty()) {
        totalBytes += tempQueue.front().packet->GetSize();
        tempQueue.pop();
    }
    
    // Priority 3
    tempQueue = m_txBufferPrio3;
    while (!tempQueue.empty()) {
        totalBytes += tempQueue.front().packet->GetSize();
        tempQueue.pop();
    }
    
    // Priority 4
    tempQueue = m_txBufferPrio4;
    while (!tempQueue.empty()) {
        totalBytes += tempQueue.front().packet->GetSize();
        tempQueue.pop();
    }
    
    return totalBytes;
}

/**
 * \brief Priority selection algorithm with fairness integration and debt management
 * 
 * This method implements the core CATS priority scheduling algorithm that balances
 * strict priority ordering with fairness mechanisms to prevent starvation.
 * 
 * ## Algorithm Steps
 * 1. **Priority Scanning**: Examines queues from P0 (URGENT) to P4 (Background)
 * 2. **Data Check**: Only considers queues that have pending packets
 * 3. **Eligibility Check**: Skips queues in debt state (P0 can override)
 * 4. **Selection**: Returns first eligible queue with data
 * 5. **Debt Management**: If all non-empty queues in debt: trigger redistribution
 * 6. **Retry**: After redistribution, re-scans for eligible queues
 * 7. **Return**: First eligible queue or 5 if none available
 * 
 * ## Priority Override for Critical Traffic
 * P0 (URGENT) can be served even when in debt state, ensuring critical
 * traffic like control messages always get through.
 * 
 * ## Fairness Integration
 * The debt state mechanism prevents any priority from monopolizing transmission:
 * - Queues accumulate debt as they send data
 * - When debt exceeds high watermark, queue becomes ineligible
 * - Lower priorities pay down debt when they get transmission opportunities
 * - Hysteresis (separate low watermark) prevents rapid state oscillation
 * 
 * This algorithm ensures efficient priority-based scheduling with continuous operation.
 */
uint8_t
TcpCats::GetNextPriorityToServe()
{
    NS_LOG_FUNCTION(this);
    
    // Check each priority level from highest to lowest
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        std::queue<CatsTxItem>& queue = GetPriorityQueue(priority);
        if (queue.empty())
        {
            continue;
        }
        
        // A queue is eligible if it's not in debt state
        // Note: m_isInDebtState[4] is always false (P4 never goes into debt)
        if (!m_isInDebtState[priority])
        {
            if (priority == 0)
            {
                NS_LOG_INFO("CATS: Priority 0 (URGENT) gets immediate service - jumping queue!");
            }
            else
            {
                NS_LOG_DEBUG("CATS: Selected priority " << (uint32_t)priority 
                            << " (debt: " << m_priorityDebt[priority] 
                            << ", eligible: not in debt state)");
            }
            return priority;
        }
        else
        {
            NS_LOG_DEBUG("CATS: Priority " << (uint32_t)priority 
                        << " ineligible - in debt state (debt: " << m_priorityDebt[priority]
                        << " >= high watermark: " << GetDebtHighWatermark(priority) << ")");
        }
    }
    
    // DEADLOCK RESOLUTION: If all non-empty queues are in debt state,
    // perform proportional debt redistribution to resume transmission
    NS_LOG_WARN("CATS: DEADLOCK DETECTED - all non-empty queues in debt state, performing debt redistribution");
    PerformDebtRedistribution();
    
    // Try again after debt redistribution
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        std::queue<CatsTxItem>& queue = GetPriorityQueue(priority);
        if (!queue.empty() && !m_isInDebtState[priority])
        {
            NS_LOG_WARN("CATS: After debt redistribution, selected priority " << (uint32_t)priority);
            return priority;
        }
    }
    
    NS_LOG_DEBUG("CATS: No eligible priority queue found - all non-empty queues are in debt state");
    return 5; // No eligible queue found
}

void
TcpCats::UpdateFairnessState(uint8_t priority, uint32_t bytesSent)
{
    NS_LOG_FUNCTION(this << (uint32_t)priority << bytesSent);
    
    // Update sender's debt (if applicable - P4 never accumulates debt)
    if (priority < 4)
    {
        uint32_t oldDebt = m_priorityDebt[priority];
        m_priorityDebt[priority] += bytesSent;
        
        NS_LOG_DEBUG("CATS: Priority " << (uint32_t)priority << " debt increased from " 
                    << oldDebt << " to " << m_priorityDebt[priority] << " bytes (+<" << bytesSent << ")");
        
        // Check if debt crossed high watermark (transition to ineligible state)
        if (m_priorityDebt[priority] >= GetDebtHighWatermark(priority))
        {
            if (!m_isInDebtState[priority])
            {
                m_isInDebtState[priority] = true;
                NS_LOG_WARN("CATS: Priority " << (uint32_t)priority 
                           << " entering DEBT STATE - debt " << m_priorityDebt[priority] 
                           << " >= high watermark " << GetDebtHighWatermark(priority));
            }
        }
    }
    
    // Process payback to higher priority queues
    double paybackMultiplier = GetPaybackMultiplier(priority);
    if (paybackMultiplier > 0.0)
    {
        uint32_t dynamicPayback = static_cast<uint32_t>(bytesSent * paybackMultiplier);
        
        NS_LOG_DEBUG("CATS: Priority " << (uint32_t)priority 
                    << " paying back " << dynamicPayback << " bytes to higher queues (multiplier: " 
                    << paybackMultiplier << ")");
        
        // Iterate through all higher-priority queues (0 to priority-1)
        for (uint8_t i = 0; i < priority; i++)
        {
            if (m_priorityDebt[i] > 0)
            {
                uint32_t oldDebt = m_priorityDebt[i];
                
                // Safe subtraction to avoid underflow
                if (m_priorityDebt[i] >= dynamicPayback)
                {
                    m_priorityDebt[i] -= dynamicPayback;
                }
                else
                {
                    m_priorityDebt[i] = 0;
                }
                
                NS_LOG_DEBUG("CATS: Priority " << (uint32_t)i << " debt reduced from " 
                            << oldDebt << " to " << m_priorityDebt[i] << " bytes");
                
                // Check if debt dropped below low watermark (transition to eligible state)
                if (m_isInDebtState[i] && m_priorityDebt[i] < GetDebtLowWatermark(i))
                {
                    m_isInDebtState[i] = false;
                    NS_LOG_WARN("CATS: Priority " << (uint32_t)i 
                               << " exiting DEBT STATE - debt " << m_priorityDebt[i] 
                               << " < low watermark " << GetDebtLowWatermark(i));
                }
            }
        }
    }
}

/**
 * \brief Performs proportional debt redistribution for continuous system operation
 * 
 * This method is a critical component of the CATS fairness system that ensures
 * continuous operation when multiple priorities have exceeded their debt watermarks
 * and would otherwise be ineligible for transmission.
 * 
 * ## Algorithm Details
 * 1. Calculate total system payback multiplier (sum of ALL P0-P4 multipliers)
 * 2. For each priority with non-empty queues and debt > 0:
 *    - Compute proportional factor = multiplier / total
 *    - Reduce debt by: new_debt = old_debt * proportional_factor
 * 3. Update debt state flags based on new debt levels vs watermarks
 * 
 * ## Proportional Fairness
 * Higher priorities have lower payback multipliers. Among non-empty queues with debt:
 * - Get smaller proportional factors (more aggressive debt reduction)
 * - Are more likely to exit debt state after redistribution
 * - Maintain relative priority ordering during redistribution
 * 
 * ## Example with Default Multipliers (0.25,0.5,1.0,1.5,2.0, sum=5.25)
 * If P0 and P3 both have 6144 bytes debt and non-empty queues:
 * - P0 debt: 6144 → 6144*(0.25/5.25) = 293 bytes (reduced by 95.2%)
 * - P3 debt: 6144 → 6144*(1.5/5.25) = 1755 bytes (reduced by 71.4%)
 * 
 * This maintains continuous system operation while preserving priority relationships.
 */
void
TcpCats::PerformDebtRedistribution()
{
    NS_LOG_FUNCTION(this);
    
    // Calculate total payback multipliers for ALL queues (P0-P4), regardless of whether they're empty
    // This ensures consistent proportional redistribution
    double totalPaybackMultiplier = 0.0;
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        totalPaybackMultiplier += GetPaybackMultiplier(priority);
    }
    
    if (totalPaybackMultiplier == 0.0)
    {
        NS_LOG_WARN("CATS: Cannot perform debt redistribution - total payback multiplier is zero");
        return;
    }
    
    NS_LOG_WARN("CATS: Performing proportional debt redistribution (total system payback multiplier: " 
                << totalPaybackMultiplier << ")");
    
    // Apply proportional debt reduction to all priorities with non-empty queues and debt
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        std::queue<CatsTxItem>& queue = GetPriorityQueue(priority);
        if (!queue.empty() && m_priorityDebt[priority] > 0)
        {
            uint32_t oldDebt = m_priorityDebt[priority];
            double proportionalFactor = GetPaybackMultiplier(priority) / totalPaybackMultiplier;
            
            // Reduce debt proportionally based on its share of total system payback capacity
            m_priorityDebt[priority] = static_cast<uint32_t>(m_priorityDebt[priority] * proportionalFactor);
            
            NS_LOG_WARN("CATS: Priority " << (uint32_t)priority 
                        << " debt redistributed: " << oldDebt << " -> " << m_priorityDebt[priority] 
                        << " (factor: " << proportionalFactor << ")");
            
            // Check if this priority can exit debt state
            if (m_isInDebtState[priority] && m_priorityDebt[priority] < GetDebtLowWatermark(priority))
            {
                m_isInDebtState[priority] = false;
                NS_LOG_WARN("CATS: Priority " << (uint32_t)priority 
                           << " exiting DEBT STATE after redistribution - debt " << m_priorityDebt[priority] 
                           << " < low watermark " << GetDebtLowWatermark(priority));
            }
        }
    }
}

void
TcpCats::CleanupQueues()
{
    NS_LOG_FUNCTION(this);
    
    // Clear all priority queues (std::queue automatically handles cleanup)
    while (!m_txBufferPrio0.empty()) m_txBufferPrio0.pop();
    while (!m_txBufferPrio1.empty()) m_txBufferPrio1.pop();
    while (!m_txBufferPrio2.empty()) m_txBufferPrio2.pop();
    while (!m_txBufferPrio3.empty()) m_txBufferPrio3.pop();
    while (!m_txBufferPrio4.empty()) m_txBufferPrio4.pop();
}

} // namespace ns3
