#include "lorawan-test-app.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/inet-socket-address.h"
#include "ns3/packet.h"
#include <cstdint>
#include <cstring>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("LoRaWanTestApp");
NS_OBJECT_ENSURE_REGISTERED (LoRaWanTestApp);

TypeId
LoRaWanTestApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::LoRaWanTestApp")
    .SetParent<Application> ()
    .AddConstructor<LoRaWanTestApp> ();
  return tid;
}

LoRaWanTestApp::LoRaWanTestApp ()
  : m_socket (0), m_downlinkSocket (0)
{
  NS_LOG_FUNCTION (this);
}

LoRaWanTestApp::~LoRaWanTestApp ()
{
  NS_LOG_FUNCTION (this);
}

void
LoRaWanTestApp::SetRemote (Address addr)
{
  m_remote = addr;
}

void
LoRaWanTestApp::StartApplication (void)
{
  NS_LOG_FUNCTION (this);
  // Create uplink socket.
  if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
      m_socket->Connect (m_remote);
    }
  // Create downlink socket to listen on port 10001.
  if (!m_downlinkSocket)
    {
      m_downlinkSocket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
      m_downlinkSocket->Bind (InetSocketAddress (Ipv4Address::GetAny (), 10001));
      m_downlinkSocket->SetRecvCallback (MakeCallback (&LoRaWanTestApp::HandleDownlink, this));
    }
  // Schedule uplink and MAC commands as before.
  Simulator::Schedule (Seconds (1.0), &LoRaWanTestApp::SendUplinkFrame, this);
  Simulator::Schedule (Seconds (3.0), &LoRaWanTestApp::SendMacCommandFrame, this);
  Simulator::Schedule (Seconds (5.0), &LoRaWanTestApp::SendTimingUpdateFrame, this);
}

void
LoRaWanTestApp::StopApplication (void)
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
}

void
LoRaWanTestApp::SendUplinkFrame (void)
{
  NS_LOG_FUNCTION (this);
  uint8_t buffer[7];
  buffer[0] = 1; // WOR type uplink.
  buffer[1] = 0x01;
  buffer[2] = 0x02;
  buffer[3] = 0x03;
  buffer[4] = 0x04;
  uint16_t frameCounter = 10;
  std::memcpy(buffer + 5, &frameCounter, sizeof(frameCounter));

  Ptr<Packet> packet = Create<Packet> (buffer, sizeof(buffer));
  NS_LOG_INFO ("Test App: Sending uplink frame.");
  m_socket->Send (packet);
}

void
LoRaWanTestApp::SendMacCommandFrame (void)
{
  NS_LOG_FUNCTION (this);
  uint8_t buffer[10];
  buffer[0] = 15; // WOR type MAC command.
  buffer[1] = 0x05;
  buffer[2] = 0x06;
  buffer[3] = 0x07;
  buffer[4] = 0x08;
  uint16_t frameCounter = 20;
  std::memcpy(buffer + 5, &frameCounter, sizeof(frameCounter));
  buffer[7] = 0x40; // Command ID for RelayConfReq.
  uint16_t newDelay = 200;
  std::memcpy(buffer + 8, &newDelay, sizeof(newDelay));

  Ptr<Packet> packet = Create<Packet> (buffer, sizeof(buffer));
  NS_LOG_INFO ("Test App: Sending MAC command frame (RelayConfReq).");
  m_socket->Send (packet);
}

void
LoRaWanTestApp::SendTimingUpdateFrame (void)
{
  NS_LOG_FUNCTION (this);
  uint8_t buffer[17];
  buffer[0] = 15; // WOR type MAC command.
  buffer[1] = 0x09; // DevAddr (example)
  buffer[2] = 0x0A;
  buffer[3] = 0x0B;
  buffer[4] = 0x0C;
  uint16_t frameCounter = 30;
  std::memcpy(buffer + 5, &frameCounter, sizeof(frameCounter));
  buffer[7] = 0x41; // Command ID for timing update.
  uint32_t cadPeriod = 1500; // 1500 ms.
  std::memcpy(buffer + 8, &cadPeriod, sizeof(cadPeriod));
  buffer[12] = 10; // CadToRx = 10 symbols.
  uint32_t tOffset = 200; // 200 ms.
  std::memcpy(buffer + 13, &tOffset, sizeof(tOffset));

  Ptr<Packet> packet = Create<Packet> (buffer, sizeof(buffer));
  NS_LOG_INFO ("Test App: Sending Timing Update MAC command frame (command 0x41).");
  m_socket->Send (packet);
}

// New: Handle downlink reception.
void
LoRaWanTestApp::HandleDownlink (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
  Ptr<Packet> packet;
  Address from;
  while ((packet = socket->RecvFrom(from)))
    {
      NS_LOG_INFO ("Test App: Received downlink packet of size " << packet->GetSize() << " bytes.");
      // Optionally, process the downlink packet (e.g., parse header, extract info).
    }
}

} // namespace ns3
