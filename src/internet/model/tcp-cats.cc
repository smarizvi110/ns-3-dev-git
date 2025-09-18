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

int
TcpCats::Send(Ptr<Packet> p, uint32_t flags)
{
    NS_LOG_FUNCTION(this << p << flags);
    
    if (!m_catsEnabled)
    {
        // If CATS is disabled, use standard TCP behavior
        return TcpSocketBase::Send(p, flags);
    }
    
    // Extract priority from packet tag
    uint8_t priority = 2; // Default priority (middle)
    PriorityTag priorityTag;
    if (p->PeekPacketTag(priorityTag))
    {
        priority = priorityTag.GetPriority();
        NS_LOG_INFO("CATS: Found priority tag with priority " << (uint32_t)priority);
    }
    else
    {
        NS_LOG_INFO("CATS: No priority tag found, using default priority " << (uint32_t)priority);
    }
    
    // For now, let's call the base class Send() to ensure proper TCP behavior
    // and add our priority management on top
    int result = TcpSocketBase::Send(p, flags);
    
    if (result > 0)
    {
        // Successfully queued - track this for priority management
        uint32_t size = p->GetSize();
        if (size > 0)
        {
            uint8_t* buffer = new uint8_t[size];
            p->CopyData(buffer, size);
            
            // Add to appropriate priority queue for future management
            EnqueueData(buffer, size, priority);
            
            delete[] buffer;
            
            NS_LOG_INFO("CATS: Queued " << size << " bytes with priority " << (uint32_t)priority);
        }
    }
    
    return result;
}

uint32_t
TcpCats::SendDataPacket(SequenceNumber32 seq, uint32_t maxSize, bool withAck)
{
    NS_LOG_FUNCTION(this << seq << maxSize << withAck);
    
    if (!m_catsEnabled)
    {
        // If CATS is disabled, use standard TCP behavior
        return TcpSocketBase::SendDataPacket(seq, maxSize, withAck);
    }
    
    NS_LOG_INFO("CATS SendDataPacket called - seq=" << seq << " maxSize=" << maxSize);
    
    // Call the base class to handle the actual packet transmission
    // Our priority tags should already be on the packets from Send() method
    uint32_t sent = TcpSocketBase::SendDataPacket(seq, maxSize, withAck);
    
    if (sent > 0)
    {
        NS_LOG_INFO("CATS: Sent " << sent << " bytes through TCP layer");
    }
    
    return sent;
}

int
TcpCats::SendWithPriority(Ptr<Packet> packet, uint8_t priority)
{
    NS_LOG_FUNCTION(this << packet << (uint32_t)priority);
    
    if (priority > 4)
    {
        NS_LOG_ERROR("Invalid CATS priority: " << (uint32_t)priority);
        return -1;
    }
    
    // Extract data from packet
    uint32_t size = packet->GetSize();
    uint8_t* buffer = new uint8_t[size];
    packet->CopyData(buffer, size);
    
    // Add to appropriate priority queue
    EnqueueData(buffer, size, priority);
    
    delete[] buffer;
    
    NS_LOG_INFO("CATS: Queued " << size << " bytes with priority " << (uint32_t)priority);
    
    return size;
}

void
TcpCats::EnqueueData(const uint8_t* data, uint32_t dataSize, uint8_t priority)
{
    NS_LOG_FUNCTION(this << dataSize << (uint32_t)priority);
    
    if (priority > 4)
    {
        NS_LOG_ERROR("Invalid priority level: " << (uint32_t)priority);
        return;
    }
    
    CatsDataItem* item = new CatsDataItem(data, dataSize);
    std::queue<CatsDataItem*>& queue = GetPriorityQueue(priority);
    queue.push(item);
    
    NS_LOG_INFO("CATS: Enqueued " << dataSize << " bytes to priority " << (uint32_t)priority 
                << " queue (now has " << queue.size() << " items)");
}

std::queue<TcpCats::CatsDataItem*>&
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

uint8_t
TcpCats::GetNextPriorityToServe()
{
    NS_LOG_FUNCTION(this);
    
    Time now = Simulator::Now();
    
    // Check each priority level from highest to lowest
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        std::queue<CatsDataItem*>& queue = GetPriorityQueue(priority);
        if (queue.empty())
        {
            continue;
        }
        
        // For priority 0, always serve immediately (highest priority)
        if (priority == 0)
        {
            NS_LOG_INFO("CATS: Serving highest priority 0");
            return priority;
        }
        
        // For lower priorities, check fairness timeout
        Time timeSinceLastSent = now - m_lastSentTime[priority];
        if (timeSinceLastSent >= m_fairnessTimeout)
        {
            NS_LOG_INFO("CATS: Serving priority " << (uint32_t)priority 
                        << " due to fairness timeout (" << timeSinceLastSent.GetMilliSeconds() << "ms)");
            return priority;
        }
        
        // If we're serving priority 0 and other priorities haven't timed out,
        // continue with priority 0
        if (priority == 0)
        {
            return priority;
        }
    }
    
    NS_LOG_INFO("CATS: No priority queue ready to serve");
    return 5; // No queue ready
}

void
TcpCats::CleanupQueues()
{
    NS_LOG_FUNCTION(this);
    
    // Clean up all priority queues
    for (uint8_t priority = 0; priority <= 4; priority++)
    {
        std::queue<CatsDataItem*>& queue = GetPriorityQueue(priority);
        while (!queue.empty())
        {
            delete queue.front();
            queue.pop();
        }
    }
}

} // namespace ns3
