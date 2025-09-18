#include "tcp-option-cats-priority.h"

#include "ns3/type-id.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("TcpOptionCatsPriority");

NS_OBJECT_ENSURE_REGISTERED (TcpOptionCatsPriority);

TcpOptionCatsPriority::TcpOptionCatsPriority ()
  : TcpOption (),
    m_priority (2)  // Default to normal priority
{
}

TcpOptionCatsPriority::~TcpOptionCatsPriority ()
{
}

TypeId
TcpOptionCatsPriority::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpOptionCatsPriority")
    .SetParent<TcpOption> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpOptionCatsPriority> ()
  ;
  return tid;
}

void
TcpOptionCatsPriority::Print (std::ostream &os) const
{
  os << "CATS Priority: " << static_cast<uint32_t> (m_priority);
}

uint32_t
TcpOptionCatsPriority::GetSerializedSize (void) const
{
  return 4;  // Kind (1) + Length (1) + Priority (1) + Reserved (1)
}

void
TcpOptionCatsPriority::Serialize (Buffer::Iterator start) const
{
  NS_LOG_INFO("TcpOptionCatsPriority::Serialize called - adding option kind 253");
  Buffer::Iterator i = start;
  i.WriteU8 (GetKind ());       // Kind = 253 (experimental)
  i.WriteU8 (4);                // Length = 4 bytes
  i.WriteU8 (m_priority);       // Priority value  
  i.WriteU8 (0);                // Reserved byte
  NS_LOG_INFO("TcpOptionCatsPriority::Serialize completed - wrote FD 04 " << (uint32_t)m_priority << " 00");
}

uint32_t
TcpOptionCatsPriority::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  uint8_t kind = i.ReadU8 ();
  if (kind != GetKind ())
    {
      NS_LOG_WARN ("Malformed CATS Priority option");
      return 0;
    }
  
  uint8_t length = i.ReadU8 ();
  if (length != 4)
    {
      NS_LOG_WARN ("Malformed CATS Priority option length");
      return 0;
    }
  
  m_priority = i.ReadU8 ();
  i.ReadU8 ();  // Skip reserved byte
  
  return 4;
}

uint8_t
TcpOptionCatsPriority::GetKind (void) const
{
  return 253;  // Use experimental option kind
}

uint8_t
TcpOptionCatsPriority::GetPriority (void) const
{
  return m_priority;
}

void
TcpOptionCatsPriority::SetPriority (uint8_t priority)
{
  if (priority <= 4)
    {
      m_priority = priority;
    }
  else
    {
      NS_LOG_WARN ("Invalid priority " << static_cast<uint32_t> (priority) << ", using default");
      m_priority = 2;
    }
}

} // namespace ns3
