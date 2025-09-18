#ifndef CATS_TCP_OPTION_PRIORITY_H
#define CATS_TCP_OPTION_PRIORITY_H

#include "ns3/tcp-option.h"

namespace ns3 {

/**
 * @brief CATS Priority TCP Option
 * 
 * This TCP option carries the priority information for CATS scheduling.
 * Format: Kind (1 byte) + Length (1 byte) + Priority (1 byte) + Reserved (1 byte)
 * Total length: 4 bytes
 */
class TcpOptionCatsPriority : public TcpOption
{
public:
  TcpOptionCatsPriority ();
  virtual ~TcpOptionCatsPriority ();

  static TypeId GetTypeId (void);

  virtual void Print (std::ostream &os) const override;
  virtual void Serialize (Buffer::Iterator start) const override;
  virtual uint32_t Deserialize (Buffer::Iterator start) override;

  virtual uint8_t GetKind (void) const override;
  virtual uint32_t GetSerializedSize (void) const override;

  /**
   * @brief Get the priority value
   * @return The priority level (0-4)
   */
  uint8_t GetPriority (void) const;

  /**
   * @brief Set the priority value  
   * @param priority The priority level (0-4, where 0 is highest)
   */
  void SetPriority (uint8_t priority);

private:
  uint8_t m_priority;  //!< Priority level (0=highest, 4=lowest)
};

} // namespace ns3

#endif // CATS_TCP_OPTION_PRIORITY_H
