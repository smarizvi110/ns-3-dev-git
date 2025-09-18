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

#ifndef TCP_CATS_H
#define TCP_CATS_H

#include "tcp-socket-base.h"
#include "ns3/traced-callback.h"
#include "ns3/ptr.h"
#include "ns3/packet.h"
#include "ns3/data-rate.h"
#include <queue>

namespace ns3
{

/**
 * \ingroup tcp
 * \brief CATS (Custody Assisted Traffic Switching) TCP Socket Implementation
 *
 * This class implements CATS TCP sockets that provide priority-based
 * packet queuing and conductor-based transmission. It inherits from
 * TcpSocketBase to preserve all standard TCP functionality including:
 * - TCP windowing and flow control
 * - Retransmission logic
 * - Congestion control integration (BBR, NewReno, etc.)
 * - Connection management
 * 
 * CATS adds priority queuing on top of the standard TCP transmission path.
 */
class TcpCats : public TcpSocketBase
{
public:
    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();

    /**
     * \brief Create an unbound TCP socket.
     */
    TcpCats();

    /**
     * \brief Clone the socket for use by TCP.
     * \return a pointer to a clone of this TcpCats instance.
     */
    Ptr<TcpSocketBase> Fork() override;

    /**
     * \brief CATS priority levels (0 = highest, 4 = lowest)
     */
    enum CatsPriority : uint8_t
    {
        PRIORITY_0 = 0, //!< Highest priority
        PRIORITY_1 = 1, //!< High priority  
        PRIORITY_2 = 2, //!< Medium priority
        PRIORITY_3 = 3, //!< Low priority
        PRIORITY_4 = 4  //!< Lowest priority
    };

protected:
    /**
     * \brief Destructor implementation.
     */
    ~TcpCats() override;

    // Override key transmission methods to add CATS priority logic
    
    /**
     * \brief Send data with CATS priority queuing
     * \param packet the packet to send
     * \param priority the CATS priority level
     * \return the number of bytes sent
     */
    virtual int SendWithPriority(Ptr<Packet> packet, uint8_t priority);

    /**
     * \brief Override Send to extract priority and queue data appropriately
     * \param p the packet to send
     * \param flags socket flags (not used)
     * \return number of bytes accepted for transmission
     */
    int Send(Ptr<Packet> p, uint32_t flags) override;

    /**
     * \brief Override SendDataPacket to use CATS conductor logic
     * \param seq sequence number to send
     * \param maxSize maximum segment size
     * \param withAck whether to send with ACK
     * \return the size of the packet sent
     */
    uint32_t SendDataPacket(SequenceNumber32 seq, uint32_t maxSize, bool withAck) override;

    /**
     * \brief Add data to appropriate priority queue
     * \param data the data to queue
     * \param dataSize size of the data
     * \param priority the priority level
     */
    virtual void EnqueueData(const uint8_t* data, uint32_t dataSize, uint8_t priority);

private:
    /**
     * \brief Data item structure for priority queues
     */
    struct CatsDataItem
    {
        uint8_t* data;        //!< Pointer to data
        uint32_t size;        //!< Size of data
        Time timestamp;       //!< When this data was queued
        
        CatsDataItem(const uint8_t* d, uint32_t s) 
            : size(s), timestamp(Simulator::Now())
        {
            data = new uint8_t[size];
            std::memcpy(data, d, size);
        }
        
        ~CatsDataItem()
        {
            delete[] data;
        }
    };

    // CATS Priority Queues (0 = highest priority, 4 = lowest)
    std::queue<CatsDataItem*> m_txBufferPrio0; //!< Priority 0 queue (highest)
    std::queue<CatsDataItem*> m_txBufferPrio1; //!< Priority 1 queue
    std::queue<CatsDataItem*> m_txBufferPrio2; //!< Priority 2 queue  
    std::queue<CatsDataItem*> m_txBufferPrio3; //!< Priority 3 queue
    std::queue<CatsDataItem*> m_txBufferPrio4; //!< Priority 4 queue (lowest)

    // Fairness mechanism - prevent starvation
    Time m_fairnessTimeout;           //!< Timeout for fairness mechanism
    Time m_lastSentTime[5];          //!< Last send time for each priority
    
    // Configuration
    bool m_catsEnabled;              //!< Whether CATS is enabled
    
    /**
     * \brief Get reference to priority queue by index
     * \param priority the priority level (0-4)
     * \return reference to the queue
     */
    std::queue<CatsDataItem*>& GetPriorityQueue(uint8_t priority);
    
    /**
     * \brief Check if any priority queue has data
     * \return true if any queue has data
     */
    bool HasQueuedData() const;
    
    /**
     * \brief Get the highest priority queue with data (considering fairness)
     * \return priority level of next queue to serve, or 5 if none
     */
    uint8_t GetNextPriorityToServe();
    
    /**
     * \brief Clean up all priority queues
     */
    void CleanupQueues();
};

} // namespace ns3

#endif /* TCP_CATS_H */
