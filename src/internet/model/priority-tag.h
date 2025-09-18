#ifndef CATS_TCP_PRIORITY_TAG_H
#define CATS_TCP_PRIORITY_TAG_H

#include "ns3/tag.h"
#include "ns3/packet.h"
#include "ns3/nstime.h"
#include <stdint.h>

namespace ns3 {

class PriorityTag : public Tag {
public:
  PriorityTag ();
  void SetPriority (uint8_t priority);
  uint8_t GetPriority () const;

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const override;
  virtual uint32_t GetSerializedSize (void) const override;
  virtual void Serialize (TagBuffer i) const override;
  virtual void Deserialize (TagBuffer i) override;
  virtual void Print (std::ostream &os) const override;

private:
  uint8_t m_priority;
};

} // namespace ns3

#endif // CATS_TCP_PRIORITY_TAG_H
