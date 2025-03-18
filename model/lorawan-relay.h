#ifndef LORAWAN_RELAY_H
#define LORAWAN_RELAY_H

#include "ns3/application.h"
#include "ns3/address.h"
#include "ns3/socket.h"
#include "ns3/core-module.h" // Use core-module instead of individual headers

namespace ns3 {

class LoRaWanRelay : public Application
{
public:
  static TypeId GetTypeId (void);
  LoRaWanRelay ();
  virtual ~LoRaWanRelay ();

  // Setters for configuration
  void SetForwardDelay (Time delay);
  void SetNetworkServerAddress (Address addr);

  // New setters for timing parameters (Phase 1)
  void SetCADPeriodicity (Time period);
  void SetCadToRx (uint32_t symbols);
  void SetTOffset (Time offset);

private:
  virtual void StartApplication (void);
  virtual void StopApplication (void);

  void HandleRead (Ptr<Socket> socket);
  void ForwardPacket (Ptr<Packet> packet);

  // New callback for downlink reception (Phase 3)
  void HandleDownlink (Ptr<Socket> socket);

  Ptr<Socket> m_socket;          // Uplink socket for receiving/sending uplink packets.
  Ptr<Socket> m_downlinkSocket;  // Downlink socket for receiving downlink packets.
  Address m_networkServerAddr;   // Network server address for uplink forwarding.
  EventId m_forwardEvent;        // Event scheduled for forwarding.
  Time m_forwardDelay;           // Delay before forwarding an uplink packet.

  // Timing parameters (Phase 1)
  Time m_CADPeriodicity;         // Time between consecutive CAD scans.
  uint32_t m_CadToRx;            // Number of symbols delay for CAD-to-RX switching.
  Time m_TOffset;                // Time offset from start of scan to end of WOR preamble.
};

} // namespace ns3

#endif // LORAWAN_RELAY_H
