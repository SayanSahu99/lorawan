#include "lorawan-relay.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/packet.h"
#include "ns3/header.h"
#include "ns3/buffer.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"
#include <cstring>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("LoRaWanRelay");
NS_OBJECT_ENSURE_REGISTERED (LoRaWanRelay);

/*
 * Simple LoRaWAN frame structure (7 bytes).
 */
struct SimpleLoRaWanFrame
{
  uint8_t worType;        // 0: Join-Request, 1: Uplink, 15: MAC Command, etc.
  uint8_t devAddr[4];     // 4 bytes DevAddr
  uint16_t frameCounter;  // 2 bytes frame counter
} __attribute__((packed));

/*
 * Simple NS-3 header wrapping our SimpleLoRaWanFrame.
 */
class SimpleLoRaWanHeader : public Header
{
public:
  SimpleLoRaWanHeader () { std::memset(&m_frame, 0, sizeof(m_frame)); }
  virtual ~SimpleLoRaWanHeader () {}

  void SetFrame (const SimpleLoRaWanFrame &frame) { m_frame = frame; }
  const SimpleLoRaWanFrame & GetFrame (void) const { return m_frame; }

  static TypeId GetTypeId (void)
  {
    static TypeId tid = TypeId ("ns3::SimpleLoRaWanHeader")
      .SetParent<Header> ()
      .AddConstructor<SimpleLoRaWanHeader> ();
    return tid;
  }
  virtual TypeId GetInstanceTypeId (void) const { return GetTypeId (); }

  virtual void Serialize (Buffer::Iterator start) const
  {
    start.WriteU8 (m_frame.worType);
    for (uint32_t i = 0; i < 4; i++)
      {
        start.WriteU8 (m_frame.devAddr[i]);
      }
    start.WriteHtonU16 (m_frame.frameCounter);
  }
  virtual uint32_t GetSerializedSize (void) const { return sizeof(SimpleLoRaWanFrame); }
  virtual uint32_t Deserialize (Buffer::Iterator start)
  {
    m_frame.worType = start.ReadU8 ();
    for (uint32_t i = 0; i < 4; i++)
      {
        m_frame.devAddr[i] = start.ReadU8 ();
      }
    m_frame.frameCounter = start.ReadNtohU16 ();
    return sizeof(SimpleLoRaWanFrame);
  }
  virtual void Print (std::ostream &os) const
  {
    os << "WOR type=" << (uint32_t)m_frame.worType
       << " DevAddr=" << std::hex
       << uint32_t(m_frame.devAddr[0]) << ":" << uint32_t(m_frame.devAddr[1]) << ":"
       << uint32_t(m_frame.devAddr[2]) << ":" << uint32_t(m_frame.devAddr[3])
       << std::dec << " FrameCounter=" << m_frame.frameCounter;
  }
private:
  SimpleLoRaWanFrame m_frame;
};

/* Helper: Parse packet into SimpleLoRaWanFrame. */
static bool
ParseLoRaWanFrame (Ptr<Packet> packet, SimpleLoRaWanFrame &frame)
{
  if (packet->GetSize () < sizeof(SimpleLoRaWanFrame))
    {
      NS_LOG_WARN ("Packet too small to parse as a LoRaWAN frame.");
      return false;
    }
  packet->CopyData (reinterpret_cast<uint8_t*>(&frame), sizeof(SimpleLoRaWanFrame));
  return true;
}

/* Placeholder: MIC verification (always passes). */
static bool
VerifyMIC (const SimpleLoRaWanFrame &frame)
{
  NS_LOG_INFO ("Verifying MIC for frame with DevAddr: "
               << std::hex
               << uint32_t(frame.devAddr[0]) << ":" 
               << uint32_t(frame.devAddr[1]) << ":" 
               << uint32_t(frame.devAddr[2]) << ":" 
               << uint32_t(frame.devAddr[3])
               << std::dec);
  return true;
}

/* Filtering: Filter out Join-Request frames (worType == 0). */
static bool
ShouldForwardFrame (const SimpleLoRaWanFrame &frame)
{
  if (frame.worType == 0)
    {
      NS_LOG_INFO ("Filtering Join-Request frame.");
      return false;
    }
  return true;
}

/*
 * Extended MAC command processing.
 * Handles command ID 0x40 (update forward delay) and 0x41 (update timing parameters).
 */
static bool
ProcessMacCommand (Ptr<Packet> packet, const SimpleLoRaWanFrame &frame, Ptr<LoRaWanRelay> relay)
{
  Ptr<Packet> extraPacket = packet->Copy();
  extraPacket->RemoveAtStart(sizeof(SimpleLoRaWanFrame));
  
  if (extraPacket->GetSize() < 1)
  {
    NS_LOG_WARN ("MAC command frame too short for command ID.");
    return false;
  }
  
  uint8_t commandId;
  extraPacket->CopyData(reinterpret_cast<uint8_t*>(&commandId), 1);
  
  NS_LOG_INFO ("MAC command received with command ID: 0x" << std::hex << (uint32_t)commandId << std::dec);
  
  if (commandId == 0x40)
  {
    if (extraPacket->GetSize() < 1 + 2)
    {
      NS_LOG_WARN ("RelayConfReq frame does not have enough data for new delay.");
      return false;
    }
    extraPacket->RemoveAtStart(1);
    uint16_t newDelayMs;
    extraPacket->CopyData(reinterpret_cast<uint8_t*>(&newDelayMs), 2);
    NS_LOG_INFO ("RelayConfReq: updating forward delay to " << newDelayMs << " ms.");
    relay->SetForwardDelay (MilliSeconds (newDelayMs));
    return true;
  }
  else if (commandId == 0x41)
  {
    // For timing update command: Expect 1 (command id) + 4 + 1 + 4 = 10 extra bytes.
    if (extraPacket->GetSize() < 1 + 9)
    {
      NS_LOG_WARN ("Timing update frame (command 0x41) too short.");
      return false;
    }
    extraPacket->RemoveAtStart(1); // Remove command ID.
    uint32_t cadPeriodMs;
    extraPacket->CopyData(reinterpret_cast<uint8_t*>(&cadPeriodMs), 4);
    extraPacket->RemoveAtStart(4);
    uint8_t cadToRx;
    extraPacket->CopyData(reinterpret_cast<uint8_t*>(&cadToRx), 1);
    extraPacket->RemoveAtStart(1);
    uint32_t tOffsetMs;
    extraPacket->CopyData(reinterpret_cast<uint8_t*>(&tOffsetMs), 4);
    
    NS_LOG_INFO ("Timing update: setting CADPeriodicity to " << cadPeriodMs 
                 << " ms, CadToRx to " << (uint32_t)cadToRx 
                 << " symbols, and TOffset to " << tOffsetMs << " ms.");
    relay->SetCADPeriodicity (MilliSeconds(cadPeriodMs));
    relay->SetCadToRx (cadToRx);
    relay->SetTOffset (MilliSeconds(tOffsetMs));
    return true;
  }
  else
  {
    NS_LOG_INFO ("Unrecognized MAC command. No action taken.");
  }
  
  return true;
}

TypeId
LoRaWanRelay::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::LoRaWanRelay")
    .SetParent<Application> ()
    .SetGroupName ("LoRaWan")
    .AddConstructor<LoRaWanRelay> ()
    .AddAttribute ("ForwardDelay",
                   "Delay before forwarding a received packet.",
                   TimeValue (MilliSeconds (100)),
                   MakeTimeAccessor (&LoRaWanRelay::m_forwardDelay),
                   MakeTimeChecker ())
    .AddAttribute ("NetworkServerAddress",
                   "Address of the network server to forward packets to.",
                   AddressValue (),
                   MakeAddressAccessor (&LoRaWanRelay::m_networkServerAddr),
                   MakeAddressChecker ())
    // Timing parameters from Phase 1:
    .AddAttribute ("CADPeriodicity",
                   "Time between consecutive CAD scans.",
                   TimeValue (Seconds (1)),
                   MakeTimeAccessor (&LoRaWanRelay::m_CADPeriodicity),
                   MakeTimeChecker ())
    .AddAttribute ("CadToRx",
                   "Number of symbols delay for CAD-to-RX mode switching.",
                   UintegerValue (8),
                   MakeUintegerAccessor (&LoRaWanRelay::m_CadToRx),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("TOffset",
                   "Time offset from start of scan to end of WOR preamble.",
                   TimeValue (Seconds (0)),
                   MakeTimeAccessor (&LoRaWanRelay::m_TOffset),
                   MakeTimeChecker ());
  return tid;
}

LoRaWanRelay::LoRaWanRelay ()
  : m_socket (0),
    m_downlinkSocket (0),
    m_forwardDelay (MilliSeconds (100)),
    m_CADPeriodicity (Seconds (1)),
    m_CadToRx (8),
    m_TOffset (Seconds (0))
{
  NS_LOG_FUNCTION (this);
}

LoRaWanRelay::~LoRaWanRelay ()
{
  NS_LOG_FUNCTION (this);
}

void
LoRaWanRelay::SetForwardDelay (Time delay)
{
  m_forwardDelay = delay;
}

void
LoRaWanRelay::SetNetworkServerAddress (Address addr)
{
  m_networkServerAddr = addr;
}

void
LoRaWanRelay::SetCADPeriodicity (Time period)
{
  m_CADPeriodicity = period;
}

void
LoRaWanRelay::SetCadToRx (uint32_t symbols)
{
  m_CadToRx = symbols;
}

void
LoRaWanRelay::SetTOffset (Time offset)
{
  m_TOffset = offset;
}

void
LoRaWanRelay::StartApplication (void)
{
  NS_LOG_FUNCTION (this);
  if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode (), TypeId::LookupByName ("ns3::UdpSocketFactory"));
      m_socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), 10000));
      m_socket->SetRecvCallback (MakeCallback (&LoRaWanRelay::HandleRead, this));
    }
  // Create a separate socket for downlink reception.
  if (!m_downlinkSocket)
    {
      m_downlinkSocket = Socket::CreateSocket (GetNode (), TypeId::LookupByName ("ns3::UdpSocketFactory"));
      m_downlinkSocket->Bind (InetSocketAddress (Ipv4Address::GetAny (), 30000));
      m_downlinkSocket->SetRecvCallback (MakeCallback (&LoRaWanRelay::HandleDownlink, this));
    }
}

void
LoRaWanRelay::StopApplication (void)
{
  NS_LOG_FUNCTION (this);
  if (m_socket)
    {
      m_socket->Close ();
      m_socket = 0;
    }
  if (m_downlinkSocket)
    {
      m_downlinkSocket->Close ();
      m_downlinkSocket = 0;
    }
  Simulator::Cancel (m_forwardEvent);
}

void
LoRaWanRelay::HandleRead (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
  Ptr<Packet> packet;
  Address from;
  while ((packet = socket->RecvFrom (from)))
    {
      NS_LOG_INFO ("Relay received a packet of size " << packet->GetSize () << " bytes");
      SimpleLoRaWanFrame frame;
      if (ParseLoRaWanFrame (packet, frame))
        {
          NS_LOG_INFO ("Parsed LoRaWAN frame: WOR type = " << (uint32_t)frame.worType
                       << ", DevAddr = " << std::hex
                       << uint32_t(frame.devAddr[0]) << ":" << uint32_t(frame.devAddr[1]) << ":"
                       << uint32_t(frame.devAddr[2]) << ":" << uint32_t(frame.devAddr[3])
                       << ", FrameCounter = " << std::dec << frame.frameCounter);
          
          if (frame.worType == 15)
            {
              NS_LOG_INFO ("MAC command detected.");
              if (!ProcessMacCommand (packet, frame, this))
                {
                  NS_LOG_WARN ("Failed to process MAC command; dropping packet.");
                  continue;
                }
              else
                {
                  NS_LOG_INFO ("MAC command processed; not forwarding the command packet.");
                  continue;
                }
            }
          
          if (!VerifyMIC (frame))
            {
              NS_LOG_WARN ("MIC verification failed. Dropping packet.");
              continue;
            }
          
          if (!ShouldForwardFrame (frame))
            {
              NS_LOG_INFO ("Frame filtered. Not forwarding.");
              continue;
            }
        }
      else
        {
          NS_LOG_WARN ("Failed to parse LoRaWAN frame from received packet.");
        }
      // Prepend header and schedule forwarding of the uplink packet.
      Ptr<Packet> clone = packet->Copy();
      SimpleLoRaWanHeader header;
      header.SetFrame(frame);
      clone->AddHeader(header);
      
      NS_LOG_INFO ("Forwarding uplink packet to network server.");
      m_forwardEvent = Simulator::Schedule (m_forwardDelay, &LoRaWanRelay::ForwardPacket, this, clone);
    }
}

void
LoRaWanRelay::HandleDownlink (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
  Ptr<Packet> packet;
  Address from;
  while ((packet = socket->RecvFrom (from)))
    {
      NS_LOG_INFO ("Relay received a downlink packet of size " << packet->GetSize () << " bytes");
      // For simplicity, forward the downlink packet to a broadcast address on port 10001.
      InetSocketAddress broadcastAddr = InetSocketAddress (Ipv4Address ("255.255.255.255"), 10001);
      NS_LOG_INFO ("Forwarding downlink packet to end-devices on port 10001.");
      m_socket->SendTo (packet, 0, broadcastAddr);
    }
}

void
LoRaWanRelay::ForwardPacket (Ptr<Packet> packet)
{
  NS_LOG_FUNCTION (this << packet);
  if (m_socket && m_networkServerAddr != Address ())
    {
      NS_LOG_INFO ("Sending uplink packet to network server.");
      m_socket->SendTo (packet, 0, m_networkServerAddr);
    }
}

} // namespace ns3
