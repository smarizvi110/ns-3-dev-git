#include "priority-tag.h"
#include "ns3/uinteger.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (PriorityTag);

PriorityTag::PriorityTag () : m_priority (0) {}

void
PriorityTag::SetPriority (uint8_t priority)
{
  m_priority = priority;
}

uint8_t
PriorityTag::GetPriority () const
{
  return m_priority;
}

TypeId
PriorityTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::PriorityTag")
    .SetParent<Tag> ()
    .AddConstructor<PriorityTag> ();
  return tid;
}

TypeId
PriorityTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

uint32_t
PriorityTag::GetSerializedSize (void) const
{
  return 1;
}

void
PriorityTag::Serialize (TagBuffer i) const
{
  i.WriteU8 (m_priority);
}

void
PriorityTag::Deserialize (TagBuffer i)
{
  m_priority = i.ReadU8 ();
}

void
PriorityTag::Print (std::ostream &os) const
{
  os << "Priority=" << static_cast<uint32_t> (m_priority);
}

} // namespace ns3
