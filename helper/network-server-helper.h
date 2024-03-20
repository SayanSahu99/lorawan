/*
 * Copyright (c) 2017 University of Padova
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Davide Magrin <magrinda@dei.unipd.it>
 *
 * Modified by: Alessandro Aimi <alessandro.aimi@unibo.it>
 */

#ifndef NETWORK_SERVER_HELPER_H
#define NETWORK_SERVER_HELPER_H

#include "ns3/address.h"
#include "ns3/application-container.h"
#include "ns3/attribute.h"
#include "ns3/net-device.h"
#include "ns3/network-server.h"
#include "ns3/node-container.h"
#include "ns3/object-factory.h"
#include "ns3/point-to-point-helper.h"

#include <stdint.h>
#include <string>

namespace ns3
{
namespace lorawan
{

/**
 * Store NS app registration details for gateway nodes having a P2P link with the NS.
 *
 * For each gateway, store in a pair:
 * - The Point-to-point net device of the network server;
 * - The gateway node connected to the P2P net device.
 */
typedef std::list<std::pair<Ptr<PointToPointNetDevice>, Ptr<Node>>> P2PGwRegistration_t;

/**
 * This class can install Network Server applications on multiple nodes at once.
 */
class NetworkServerHelper
{
  public:
    NetworkServerHelper();

    ~NetworkServerHelper();

    void SetAttribute(std::string name, const AttributeValue& value);

    ApplicationContainer Install(Ptr<Node> node);

    /**
     * Register gateways connected with point-to-point to this NS.
     *
     * \remark For the moment, only P2P connections are supported.
     *
     * \param registration The gateways registration data.
     *
     * \see ns3::lorawan::P2PGwRegistration_t
     */
    void SetGatewaysP2P(const P2PGwRegistration_t& registration);

    /**
     * Set which end devices will be managed by this NS.
     */
    void SetEndDevices(NodeContainer endDevices);

    /**
     * Enable (true) or disable (false) the ADR component in the Network
     * Server created by this helper.
     */
    void EnableAdr(bool enableAdr);

    /**
     * Set the ADR implementation to use in the Network Server created
     * by this helper.
     */
    void SetAdr(std::string type);

  private:
    void InstallComponents(Ptr<NetworkServer> netServer);
    Ptr<Application> InstallPriv(Ptr<Node> node);

    ObjectFactory m_factory;

    std::list<std::pair<Ptr<NetDevice>, Ptr<Node>>>
        m_gatewayRegistrationList; //!< List of gateway nodes to register to this NS net devices

    NodeContainer m_endDevices; //!< Set of endDevices to connect to this NS

    bool m_adrEnabled;

    ObjectFactory m_adrSupportFactory;
};

} // namespace lorawan

} // namespace ns3
#endif /* NETWORK_SERVER_HELPER_H */
