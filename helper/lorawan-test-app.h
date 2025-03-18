#ifndef LORAWAN_TEST_APP_H
#define LORAWAN_TEST_APP_H

#include "ns3/application.h"
#include "ns3/address.h"
#include "ns3/socket.h"
#include "ns3/core-module.h" // For Time, etc.

namespace ns3 {

class LoRaWanTestApp : public Application
{
public:
  static TypeId GetTypeId (void);
  LoRaWanTestApp ();
  virtual ~LoRaWanTestApp ();

  void SetRemote (Address addr);

private:
  virtual void StartApplication (void);
  virtual void StopApplication (void);

  void SendUplinkFrame (void);
  void SendMacCommandFrame (void);
  void SendTimingUpdateFrame (void);
  // New: Downlink reception callback.
  void HandleDownlink (Ptr<Socket> socket);

  Ptr<Socket> m_socket;
  Ptr<Socket> m_downlinkSocket;  // For receiving downlink messages.
  Address m_remote;
};

} // namespace ns3

#endif // LORAWAN_TEST_APP_H
