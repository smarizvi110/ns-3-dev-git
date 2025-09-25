/*
 * Copyright (c) 2025
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * b     * This method is called when all non-empty queues are in debt state,
     * which would normally prevent transmission. The redistribution
     * uses proportional payback multipliers to reduce debts simultaneously
     * for all priorities that have both non-empty queues and debt > 0:
     * 
     * For each priority i with non-empty queue and debt > 0: 
     *   reduction_factor = payback_multiplier[i] / sum_of_all_multipliers
     *   new_debt[i] = old_debt[i] * reduction_factorOUT ANY WARRANTY; without even the implied warranty of
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
 * packet queuing with sophisticated fairness mechanisms and conductor-based 
 * transmission. It inherits from TcpSocketBase to preserve all standard TCP 
 * functionality including:
 * - TCP windowing and flow control
 * - Retransmission logic
 * - Congestion control integration (BBR, NewReno, etc.)
 * - Connection management
 * 
 * ## CATS Architecture: "Interceptor and Feeder"
 * 
 * **INTERCEPTOR**: The Send() method intercepts application data and routes it
 * to priority-specific queues (P0-P4) instead of directly to TCP buffer.
 * 
 * **CONDUCTOR**: The ConductorFeedData() method implements segment-by-segment
 * feeding from priority queues to the base TCP layer, with continuous priority
 * re-evaluation after each segment transmission.
 * 
 * **FEEDER**: Base TCP layer handles actual network transmission timing,
 * flow control, and congestion control.
 * 
 * ## Priority System (5 Levels)
 * - P0 (URGENT): Highest priority, immediate service, queue jumping
 * - P1 (Interactive): High priority, low latency applications  
 * - P2 (Control): Medium priority, control plane traffic
 * - P3 (Bulk): Background priority, bulk data transfers
 * - P4 (Background): Lowest priority, best-effort traffic
 * 
 * ## Credit-Based Fairness System
 * 
 * To prevent lower priorities from being completely starved by higher priorities,
 * CATS implements a sophisticated credit-based fairness mechanism with:
 * 
 * **Debt Tracking**: Each priority accumulates "debt" as it sends data.
 * When debt exceeds configurable high watermarks, the priority becomes
 * ineligible for transmission until debt drops below low watermarks.
 * 
 * **Hysteresis Control**: Separate high/low watermarks prevent oscillation
 * and provide stable fairness behavior.
 * 
 * **Proportional Payback**: Each priority has configurable payback multipliers
 * that determine how quickly debt is reduced when lower priorities send data.
 * Higher priorities have lower multipliers (pay back debt slower).
 * 
 * **Continuous Operation**: When all non-empty queues are in debt state,
 * the system performs proportional debt redistribution using system-wide
 * payback multiplier totals to ensure forward progress.
 * 
 * ## Segment-by-Segment Feeding
 * 
 * The conductor uses segment-by-segment feeding (not bulk queue draining)
 * to ensure proper priority re-evaluation. After each segment is fed to TCP,
 * the conductor recalculates which priority should be served next, enabling
 * true priority jumping behavior where urgent traffic can interrupt ongoing
 * lower priority transmission.
 * 
 * ## Configuration Parameters
 * 
 * All watermarks and payback multipliers are configurable via TypeId attributes:
 * - DebtHighWatermarkP0-P3: High debt thresholds (bytes)
 * - DebtLowWatermarkP0-P3: Low debt thresholds (bytes) 
 * - PaybackMultiplierP0-P4: Debt reduction multipliers
 * 
 * Default configuration provides balanced fairness while preserving priority ordering.
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
     * \brief CATS Conductor - feeds data from priority queues to base class buffer
     * 
     * This method implements the core CATS feeding logic with segment-by-segment
     * transmission and continuous priority re-evaluation. Key behaviors:
     * 
     * - Uses if-statement (not while-loop) to feed only ONE segment per call
     * - Allows priority re-evaluation after each segment transmission
     * - Enables true priority jumping where higher priority traffic can
     *   interrupt ongoing lower priority transmission
     * - Integrates with TCP flow control via GetTxAvailable()
     * - Updates fairness state after each successful transmission
     * - Handles debt redistribution for continuous operation
     * 
     * Called automatically after ACK reception to maintain transmission flow.
     */
    void ConductorFeedData();

    /**
     * \brief Override SetSndBufSize to manage CATS buffer limits
     * \param size the new buffer size
     */
    void SetSndBufSize(uint32_t size) override;

    /**
     * \brief Override ReceivedAck to trigger Conductor after ACK processing
     * \param packet the received packet
     * \param tcpHeader the TCP header
     */
    void ReceivedAck(Ptr<Packet> packet, const TcpHeader& tcpHeader) override;

private:
    /**
     * \brief Transmission item structure for priority queues
     */
    struct CatsTxItem
    {
        Ptr<Packet> packet;      //!< The packet to transmit
        Time enqueueTime;        //!< When this item was queued
        
        CatsTxItem(Ptr<Packet> p) 
            : packet(p), enqueueTime(Simulator::Now())
        {
        }
    };

    // CATS Priority Queues (0 = highest priority, 4 = lowest)
    std::queue<CatsTxItem> m_txBufferPrio0; //!< Priority 0 queue (highest)
    std::queue<CatsTxItem> m_txBufferPrio1; //!< Priority 1 queue
    std::queue<CatsTxItem> m_txBufferPrio2; //!< Priority 2 queue  
    std::queue<CatsTxItem> m_txBufferPrio3; //!< Priority 3 queue
    std::queue<CatsTxItem> m_txBufferPrio4; //!< Priority 4 queue (lowest)

    // Buffer management - rely on base TCP buffer through GetTxAvailable()
    
    // Sophisticated Credit-Based Fairness Mechanism with Hysteresis
    uint32_t m_priorityDebt[5];           //!< Current debt in bytes for each priority level
    bool m_isInDebtState[5];              //!< Whether each queue is currently in debt state (ineligible)
    
    // Configurable watermark thresholds (P0-P3, P4 uses defaults)
    uint32_t m_debtHighWatermarkP0;       //!< P0 high debt threshold 
    uint32_t m_debtHighWatermarkP1;       //!< P1 high debt threshold
    uint32_t m_debtHighWatermarkP2;       //!< P2 high debt threshold
    uint32_t m_debtHighWatermarkP3;       //!< P3 high debt threshold
    uint32_t m_debtLowWatermarkP0;        //!< P0 low debt threshold
    uint32_t m_debtLowWatermarkP1;        //!< P1 low debt threshold
    uint32_t m_debtLowWatermarkP2;        //!< P2 low debt threshold
    uint32_t m_debtLowWatermarkP3;        //!< P3 low debt threshold
    
    // Configurable payback multipliers (P0-P4)
    double m_paybackMultiplierP0;         //!< P0 payback multiplier (for debt redistribution)
    double m_paybackMultiplierP1;         //!< P1 payback multiplier
    double m_paybackMultiplierP2;         //!< P2 payback multiplier
    double m_paybackMultiplierP3;         //!< P3 payback multiplier
    double m_paybackMultiplierP4;         //!< P4 payback multiplier
    
    // Configuration
    bool m_catsEnabled;              //!< Whether CATS is enabled
    uint8_t m_lastServedPriority;    //!< Track what priority was last being served
    bool m_conductorActive;          //!< Whether conductor is currently running
    
    /**
     * \brief Get reference to priority queue by index
     * \param priority the priority level (0-4)
     * \return reference to the queue
     */
    std::queue<CatsTxItem>& GetPriorityQueue(uint8_t priority);
    
    /**
     * \brief Check if any priority queue has data
     * \return true if any queue has data
     */
    bool HasQueuedData() const;
    
    /**
     * \brief Get total bytes queued across all priority queues
     * \return total bytes in all CATS priority queues
     */
    uint32_t GetTotalQueuedBytes() const;
    
    /**
     * \brief Get the highest priority eligible queue with data
     * 
     * This method implements the core priority selection algorithm:
     * 
     * 1. **Priority Scanning**: Examines queues from P0 (highest) to P4 (lowest)
     * 2. **Eligibility Check**: Queues in debt state are ineligible for transmission
     * 3. **Data Availability**: Only considers queues that have pending data
     * 4. **Debt Management**: If all non-empty queues are in debt state,
     *    triggers debt redistribution to ensure forward progress
     * 5. **P0 Override**: Priority 0 (URGENT) can be served even in debt state
     *    for critical traffic handling
     * 
     * \return priority level of next queue to serve, or 5 if none eligible
     */
    uint8_t GetNextPriorityToServe();
    
    /**
     * \brief Update fairness state after successful transmission
     * 
     * This method updates the credit-based fairness system after data transmission:
     * 
     * 1. **Debt Accumulation**: Increases debt for the transmitting priority
     * 2. **Debt Payback**: Reduces debt for all OTHER priorities using their
     *    respective payback multipliers (higher priorities pay back slower)  
     * 3. **State Transitions**: Updates debt state flags based on watermarks:
     *    - Enters debt state when debt exceeds high watermark
     *    - Exits debt state when debt drops below low watermark
     * 4. **Hysteresis Control**: Separate thresholds prevent rapid state oscillation
     * 
     * \param priority the priority level that just sent data
     * \param bytesSent the number of bytes that were sent
     */
    void UpdateFairnessState(uint8_t priority, uint32_t bytesSent);
    
    /**
     * \brief Perform proportional debt redistribution for continuous operation
     * 
     * This method is called when all non-empty queues are in debt state,
     * which would normally prevent transmission. The redistribution
     * uses proportional payback multipliers to reduce all debts simultaneously:
     * 
     * For each priority i: 
     *   proportional_factor = payback_multiplier[i] / sum_of_all_multipliers
     *   new_debt[i] = old_debt[i] * proportional_factor
     * 
     * This ensures that higher priorities (lower multipliers) get more aggressive
     * debt reduction and are more likely to exit debt state, preserving priority ordering
     * while ensuring continuous operation.
     * 
     * Example with default multipliers (0.25,0.5,1.0,1.5,2.0, sum=5.25):
     * If P0 and P3 both have non-empty queues with 6144 bytes debt:
     * - P0: new debt = 6144 * (0.25/5.25) = 293 bytes (95.2% reduction)
     * - P3: new debt = 6144 * (1.5/5.25) = 1755 bytes (71.4% reduction)
     * 
     * This maintains relative fairness while ensuring forward progress.
     */
    void PerformDebtRedistribution();
    
    /**
     * \brief Get debt high watermark for a priority level
     * \param priority the priority level (0-4)
     * \return the high watermark threshold in bytes
     */
    uint32_t GetDebtHighWatermark(uint8_t priority) const;
    
    /**
     * \brief Get debt low watermark for a priority level  
     * \param priority the priority level (0-4)
     * \return the low watermark threshold in bytes
     */
    uint32_t GetDebtLowWatermark(uint8_t priority) const;
    
    /**
     * \brief Get payback multiplier for a priority level
     * \param priority the priority level (0-4)
     * \return the payback multiplier
     */
    double GetPaybackMultiplier(uint8_t priority) const;
    
    /**
     * \brief Clean up all priority queues
     */
    void CleanupQueues();
};

} // namespace ns3

#endif /* TCP_CATS_H */
