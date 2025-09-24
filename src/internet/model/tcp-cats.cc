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
                            .AddAttribute("FairnessTimeout",
                                        "Timeout for CATS fairness mechanism",
                                        TimeValue(MilliSeconds(100)),
                                        MakeTimeAccessor(&TcpCats::m_fairnessTimeout),
                                        MakeTimeChecker());
    return tid;
}

TcpCats::TcpCats()
    : TcpSocketBase(),
      m_fairnessTimeout(MilliSeconds(100)),
      m_catsEnabled(true)
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("TcpCats socket created - CATS priority queuing enabled");
    
    // Initialize last sent times
    for (int i = 0; i < 5; i++)
    {
        m_lastSentTime[i] = Seconds(0);
    }
}

TcpCats::~TcpCats()
{
    NS_LOG_FUNCTION(this);
    CleanupQueues();
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
    
    if (wasCompletelyEmpty)
    {
        NS_LOG_WARN("CATS: 🔄 INTERCEPTOR RESTART: First data arrival after complete queue emptying");
    }
    
    NS_LOG_INFO("CATS: Interceptor accepted " << packetSize << " bytes with priority " << (uint32_t)priority 
                << " (base TCP buffer available: " << GetTxAvailable() << " bytes, total queued data in CATS: " 
                << GetTotalQueuedBytes() << " bytes)");
    
    // Trigger the Conductor to start feeding data to the base class
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
    
    // Continue feeding the base class buffer as long as:
    // 1. We have data in our priority queues
    // 2. The base class buffer has space
    while (HasQueuedData())
    {
        // Check if base class buffer has space
        if (GetTxAvailable() == 0)
        {
            NS_LOG_INFO("CATS: Conductor paused - base TCP buffer full. "
                        << GetTotalQueuedBytes() << " bytes remain in CATS queues awaiting TCP transmission");
            break;
        }
        
        // Perform priority and fairness selection
        uint8_t selectedPriority = GetNextPriorityToServe();
        if (selectedPriority > 4)
        {
            NS_LOG_DEBUG("CATS: No eligible priority queue found");
            break;
        }
        
        // Get the selected queue
        std::queue<CatsTxItem>& selectedQueue = GetPriorityQueue(selectedPriority);
        if (selectedQueue.empty())
        {
            NS_LOG_ERROR("CATS: Selected queue is empty - this should not happen");
            break;
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
            break;
        }
        
        NS_LOG_DEBUG("CATS: Feeder delivered " << sentBytes << " bytes from priority " << (uint32_t)selectedPriority 
                    << " to base TCP (base buffer available: " << GetTxAvailable() 
                    << ", CATS queue remaining: " << GetTotalQueuedBytes() << " bytes)");
        
        // Update fairness timestamp
        m_lastSentTime[selectedPriority] = Simulator::Now();
        
        // Remove the sent bytes from the original packet
        largePacket->RemoveAtStart(sentBytes);
        
        // If the original packet is now empty, remove it from the queue
        if (largePacket->GetSize() == 0)
        {
            selectedQueue.pop();
            NS_LOG_DEBUG("CATS: Priority " << (uint32_t)selectedPriority << " packet fully transmitted");
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

uint8_t
TcpCats::GetNextPriorityToServe()
{
    NS_LOG_FUNCTION(this);
    
    Time now = Simulator::Now();
    
    // Check each priority level from highest to lowest
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        std::queue<CatsTxItem>& queue = GetPriorityQueue(priority);
        if (queue.empty())
        {
            continue;
        }
        
        // For priority 0, always serve immediately (highest priority)
        if (priority == 0)
        {
            NS_LOG_INFO("CATS: Priority 0 (URGENT) gets immediate service - jumping queue!");
            return priority;
        }
        
        // For lower priorities, check fairness timeout
        Time timeSinceLastSent = now - m_lastSentTime[priority];
        if (timeSinceLastSent >= m_fairnessTimeout)
        {
            NS_LOG_DEBUG("CATS: Fairness mechanism activating priority " << (uint32_t)priority 
                        << " (waited " << timeSinceLastSent.GetMilliSeconds() << "ms >= " 
                        << m_fairnessTimeout.GetMilliSeconds() << "ms timeout)");
            return priority;
        }
        
        // If we're serving priority 0 and other priorities haven't timed out,
        // continue with priority 0
        if (priority == 0)
        {
            return priority;
        }
    }
    
    NS_LOG_DEBUG("CATS: All queues waiting for fairness timeout - no queue ready to serve");
    return 5; // No queue ready
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
