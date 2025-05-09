/*
 * Copyright (c) 2017 University of Padova
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: Davide Magrin <magrinda@dei.unipd.it>
 *         Martina Capuzzo <capuzzom@dei.unipd.it>
 *
 * Modified by: Peggy Anderson <peggy.anderson@usask.ca>
 *              qiuyukang <b612n@qq.com>
 */

#include "class-a-end-device-lorawan-mac.h"

#include "end-device-lora-phy.h"
#include "end-device-lorawan-mac.h"

#include "ns3/log.h"

#include <algorithm>

namespace ns3
{
namespace lorawan
{

NS_LOG_COMPONENT_DEFINE("ClassAEndDeviceLorawanMac");

NS_OBJECT_ENSURE_REGISTERED(ClassAEndDeviceLorawanMac);

TypeId
ClassAEndDeviceLorawanMac::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ClassAEndDeviceLorawanMac")
                            .SetParent<EndDeviceLorawanMac>()
                            .SetGroupName("lorawan")
                            .AddConstructor<ClassAEndDeviceLorawanMac>();
    return tid;
}

ClassAEndDeviceLorawanMac::ClassAEndDeviceLorawanMac()
    : // LoraWAN default
      m_receiveDelay1(Seconds(1)),
      // LoraWAN default
      m_receiveDelay2(Seconds(2)),
      m_rx1DrOffset(0)
{
    NS_LOG_FUNCTION(this);

    // Void the two receiveWindow events
    m_closeFirstWindow = EventId();
    m_closeFirstWindow.Cancel();
    m_closeSecondWindow = EventId();
    m_closeSecondWindow.Cancel();
    m_secondReceiveWindow = EventId();
    m_secondReceiveWindow.Cancel();
}

ClassAEndDeviceLorawanMac::~ClassAEndDeviceLorawanMac()
{
    NS_LOG_FUNCTION_NOARGS();
}

/////////////////////
// Sending methods //
/////////////////////

void
ClassAEndDeviceLorawanMac::SendToPhy(Ptr<Packet> packetToSend)
{
    /////////////////////////////////////////////////////////
    // Add headers, prepare TX parameters and send the packet
    /////////////////////////////////////////////////////////

    NS_LOG_DEBUG("PacketToSend: " << packetToSend);

    // Data rate adaptation as in LoRaWAN specification, V1.0.2 (2016)
    if (m_enableDRAdapt && (m_dataRate > 0) && (m_retxParams.retxLeft < m_maxNumbTx) &&
        (m_retxParams.retxLeft % 2 == 0))
    {
        m_txPower = 14; // Reset transmission power
        m_dataRate = m_dataRate - 1;
    }

    // Craft LoraTxParameters object
    LoraTxParameters params;
    params.sf = GetSfFromDataRate(m_dataRate);
    params.headerDisabled = m_headerDisabled;
    params.codingRate = m_codingRate;
    params.bandwidthHz = GetBandwidthFromDataRate(m_dataRate);
    params.nPreamble = m_nPreambleSymbols;
    params.crcEnabled = true;
    params.lowDataRateOptimizationEnabled = LoraPhy::GetTSym(params) > MilliSeconds(16);

    // Wake up PHY layer and directly send the packet

    Ptr<LogicalLoraChannel> txChannel = GetChannelForTx();

    NS_LOG_DEBUG("PacketToSend: " << packetToSend);
    m_phy->Send(packetToSend, params, txChannel->GetFrequency(), m_txPower);

    //////////////////////////////////////////////
    // Register packet transmission for duty cycle
    //////////////////////////////////////////////

    // Compute packet duration
    Time duration = LoraPhy::GetOnAirTime(packetToSend, params);

    // Register the sent packet into the DutyCycleHelper
    m_channelHelper->AddEvent(duration, txChannel);

    //////////////////////////////
    // Prepare for the downlink //
    //////////////////////////////

    // Switch the PHY to the channel so that it will listen here for downlink
    DynamicCast<EndDeviceLoraPhy>(m_phy)->SetFrequency(txChannel->GetFrequency());

    // Instruct the PHY on the right Spreading Factor to listen for during the window
    // create a SetReplyDataRate function?
    uint8_t replyDataRate = GetFirstReceiveWindowDataRate();
    NS_LOG_DEBUG("m_dataRate: " << unsigned(m_dataRate)
                                << ", m_rx1DrOffset: " << unsigned(m_rx1DrOffset)
                                << ", replyDataRate: " << unsigned(replyDataRate) << ".");

    DynamicCast<EndDeviceLoraPhy>(m_phy)->SetSpreadingFactor(GetSfFromDataRate(replyDataRate));
}

//////////////////////////
//  Receiving methods   //
//////////////////////////
void
ClassAEndDeviceLorawanMac::Receive(Ptr<const Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);

    // Work on a copy of the packet
    Ptr<Packet> packetCopy = packet->Copy();

    // Remove the Mac Header to get some information
    LorawanMacHeader mHdr;
    packetCopy->RemoveHeader(mHdr);

    NS_LOG_DEBUG("Mac Header: " << mHdr);

    // Only keep analyzing the packet if it's downlink
    if (!mHdr.IsUplink())
    {
        NS_LOG_INFO("Found a downlink packet.");

        // Remove the Frame Header
        LoraFrameHeader fHdr;
        fHdr.SetAsDownlink();
        packetCopy->RemoveHeader(fHdr);

        NS_LOG_DEBUG("Frame Header: " << fHdr);

        // Determine whether this packet is for us
        bool messageForUs = (m_address == fHdr.GetAddress());

        if (messageForUs)
        {
            NS_LOG_INFO("The message is for us!");

            // If it exists, cancel the second receive window event
            // THIS WILL BE GetReceiveWindow()
            Simulator::Cancel(m_secondReceiveWindow);

            // Parse the MAC commands
            ParseCommands(fHdr);

            // TODO Pass the packet up to the NetDevice

            // Call the trace source
            m_receivedPacket(packet);
        }
        else
        {
            NS_LOG_DEBUG("The message is intended for another recipient.");

            // In this case, we are either receiving in the first receive window
            // and finishing reception inside the second one, or receiving a
            // packet in the second receive window and finding out, after the
            // fact, that the packet is not for us. In either case, if we no
            // longer have any retransmissions left, we declare failure.
            if (m_retxParams.waitingAck && m_secondReceiveWindow.IsExpired())
            {
                if (m_retxParams.retxLeft == 0)
                {
                    uint8_t txs = m_maxNumbTx - (m_retxParams.retxLeft);
                    m_requiredTxCallback(txs,
                                         false,
                                         m_retxParams.firstAttempt,
                                         m_retxParams.packet);
                    NS_LOG_DEBUG("Failure: no more retransmissions left. Used "
                                 << unsigned(txs) << " transmissions.");

                    // Reset retransmission parameters
                    resetRetransmissionParameters();
                }
                else // Reschedule
                {
                    this->Send(m_retxParams.packet);
                    NS_LOG_INFO("We have " << unsigned(m_retxParams.retxLeft)
                                           << " retransmissions left: rescheduling transmission.");
                }
            }
        }
    }
    else if (m_retxParams.waitingAck && m_secondReceiveWindow.IsExpired())
    {
        NS_LOG_INFO("The packet we are receiving is in uplink.");
        if (m_retxParams.retxLeft > 0)
        {
            this->Send(m_retxParams.packet);
            NS_LOG_INFO("We have " << unsigned(m_retxParams.retxLeft)
                                   << " retransmissions left: rescheduling transmission.");
        }
        else
        {
            uint8_t txs = m_maxNumbTx - (m_retxParams.retxLeft);
            m_requiredTxCallback(txs, false, m_retxParams.firstAttempt, m_retxParams.packet);
            NS_LOG_DEBUG("Failure: no more retransmissions left. Used " << unsigned(txs)
                                                                        << " transmissions.");

            // Reset retransmission parameters
            resetRetransmissionParameters();
        }
    }

    DynamicCast<EndDeviceLoraPhy>(m_phy)->SwitchToSleep();
}

void
ClassAEndDeviceLorawanMac::FailedReception(Ptr<const Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);

    // Switch to sleep after a failed reception
    DynamicCast<EndDeviceLoraPhy>(m_phy)->SwitchToSleep();

    if (m_secondReceiveWindow.IsExpired() && m_retxParams.waitingAck)
    {
        if (m_retxParams.retxLeft > 0)
        {
            this->Send(m_retxParams.packet);
            NS_LOG_INFO("We have " << unsigned(m_retxParams.retxLeft)
                                   << " retransmissions left: rescheduling transmission.");
        }
        else
        {
            uint8_t txs = m_maxNumbTx - (m_retxParams.retxLeft);
            m_requiredTxCallback(txs, false, m_retxParams.firstAttempt, m_retxParams.packet);
            NS_LOG_DEBUG("Failure: no more retransmissions left. Used " << unsigned(txs)
                                                                        << " transmissions.");

            // Reset retransmission parameters
            resetRetransmissionParameters();
        }
    }
}

void
ClassAEndDeviceLorawanMac::TxFinished(Ptr<const Packet> packet)
{
    NS_LOG_FUNCTION_NOARGS();

    // Schedule the opening of the first receive window
    Simulator::Schedule(m_receiveDelay1, &ClassAEndDeviceLorawanMac::OpenFirstReceiveWindow, this);

    // Schedule the opening of the second receive window
    m_secondReceiveWindow = Simulator::Schedule(m_receiveDelay2,
                                                &ClassAEndDeviceLorawanMac::OpenSecondReceiveWindow,
                                                this);
    // // Schedule the opening of the first receive window
    // Simulator::Schedule (m_receiveDelay1,
    //                      &ClassAEndDeviceLorawanMac::OpenFirstReceiveWindow, this);
    //
    // // Schedule the opening of the second receive window
    // m_secondReceiveWindow = Simulator::Schedule (m_receiveDelay2,
    //                                              &ClassAEndDeviceLorawanMac::OpenSecondReceiveWindow,
    //                                              this);

    // Switch the PHY to sleep
    DynamicCast<EndDeviceLoraPhy>(m_phy)->SwitchToSleep();
}

void
ClassAEndDeviceLorawanMac::OpenFirstReceiveWindow()
{
    NS_LOG_FUNCTION_NOARGS();

    // Set Phy in Standby mode
    DynamicCast<EndDeviceLoraPhy>(m_phy)->SwitchToStandby();

    // Calculate the duration of a single symbol for the first receive window data rate
    double tSym = pow(2, GetSfFromDataRate(GetFirstReceiveWindowDataRate())) /
                  GetBandwidthFromDataRate(GetFirstReceiveWindowDataRate());

    // Schedule return to sleep after "at least the time required by the end
    // device's radio transceiver to effectively detect a downlink preamble"
    // (LoraWAN specification)
    m_closeFirstWindow = Simulator::Schedule(Seconds(m_receiveWindowDurationInSymbols * tSym),
                                             &ClassAEndDeviceLorawanMac::CloseFirstReceiveWindow,
                                             this); // m_receiveWindowDuration
}

void
ClassAEndDeviceLorawanMac::CloseFirstReceiveWindow()
{
    NS_LOG_FUNCTION_NOARGS();

    Ptr<EndDeviceLoraPhy> phy = DynamicCast<EndDeviceLoraPhy>(m_phy);

    // Check the Phy layer's state:
    // - RX -> We are receiving a preamble.
    // - STANDBY -> Nothing was received.
    // - SLEEP -> We have received a packet.
    // We should never be in TX or SLEEP mode at this point
    switch (phy->GetState())
    {
    case EndDeviceLoraPhy::TX:
        NS_ABORT_MSG("PHY was in TX mode when attempting to close a receive window.");
        break;
    case EndDeviceLoraPhy::RX:
        // PHY is receiving: let it finish. The Receive method will switch it back to SLEEP.
    case EndDeviceLoraPhy::SLEEP:
        // PHY has received, and the MAC's Receive already put the device to sleep
        break;
    case EndDeviceLoraPhy::STANDBY:
        // Turn PHY layer to SLEEP
        phy->SwitchToSleep();
        break;
    }
}

void
ClassAEndDeviceLorawanMac::OpenSecondReceiveWindow()
{
    NS_LOG_FUNCTION_NOARGS();

    // Check for receiver status: if it's locked on a packet, don't open this
    // window at all.
    if (DynamicCast<EndDeviceLoraPhy>(m_phy)->GetState() == EndDeviceLoraPhy::RX)
    {
        NS_LOG_INFO("Won't open second receive window since we are in RX mode.");

        return;
    }

    // Set Phy in Standby mode
    DynamicCast<EndDeviceLoraPhy>(m_phy)->SwitchToStandby();

    // Switch to appropriate channel and data rate
    NS_LOG_INFO("Using parameters: " << m_secondReceiveWindowFrequency << "Hz, DR"
                                     << unsigned(m_secondReceiveWindowDataRate));

    DynamicCast<EndDeviceLoraPhy>(m_phy)->SetFrequency(m_secondReceiveWindowFrequency);
    DynamicCast<EndDeviceLoraPhy>(m_phy)->SetSpreadingFactor(
        GetSfFromDataRate(m_secondReceiveWindowDataRate));

    // Calculate the duration of a single symbol for the second receive window data rate
    double tSym = pow(2, GetSfFromDataRate(GetSecondReceiveWindowDataRate())) /
                  GetBandwidthFromDataRate(GetSecondReceiveWindowDataRate());

    // Schedule return to sleep after "at least the time required by the end
    // device's radio transceiver to effectively detect a downlink preamble"
    // (LoraWAN specification)
    m_closeSecondWindow = Simulator::Schedule(Seconds(m_receiveWindowDurationInSymbols * tSym),
                                              &ClassAEndDeviceLorawanMac::CloseSecondReceiveWindow,
                                              this);
}

void
ClassAEndDeviceLorawanMac::CloseSecondReceiveWindow()
{
    NS_LOG_FUNCTION_NOARGS();

    Ptr<EndDeviceLoraPhy> phy = DynamicCast<EndDeviceLoraPhy>(m_phy);

    // NS_ASSERT (phy->m_state != EndDeviceLoraPhy::TX &&
    // phy->m_state != EndDeviceLoraPhy::SLEEP);

    // Check the Phy layer's state:
    // - RX -> We have received a preamble.
    // - STANDBY -> Nothing was detected.
    switch (phy->GetState())
    {
    case EndDeviceLoraPhy::TX:
    case EndDeviceLoraPhy::SLEEP:
        break;
    case EndDeviceLoraPhy::RX:
        // PHY is receiving: let it finish
        NS_LOG_DEBUG("PHY is receiving: Receive will handle the result.");
        return;
    case EndDeviceLoraPhy::STANDBY:
        // Turn PHY layer to sleep
        phy->SwitchToSleep();
        break;
    }

    if (m_retxParams.waitingAck)
    {
        NS_LOG_DEBUG("No reception initiated by PHY: rescheduling transmission.");
        if (m_retxParams.retxLeft > 0)
        {
            NS_LOG_INFO("We have " << unsigned(m_retxParams.retxLeft)
                                   << " retransmissions left: rescheduling transmission.");
            this->Send(m_retxParams.packet);
        }

        else if (m_retxParams.retxLeft == 0 &&
                 DynamicCast<EndDeviceLoraPhy>(m_phy)->GetState() != EndDeviceLoraPhy::RX)
        {
            uint8_t txs = m_maxNumbTx - (m_retxParams.retxLeft);
            m_requiredTxCallback(txs, false, m_retxParams.firstAttempt, m_retxParams.packet);
            NS_LOG_DEBUG("Failure: no more retransmissions left. Used " << unsigned(txs)
                                                                        << " transmissions.");

            // Reset retransmission parameters
            resetRetransmissionParameters();
        }

        else
        {
            NS_ABORT_MSG("The number of retransmissions left is negative ! ");
        }
    }
    else
    {
        uint8_t txs = m_maxNumbTx - (m_retxParams.retxLeft);
        m_requiredTxCallback(txs, true, m_retxParams.firstAttempt, m_retxParams.packet);
        NS_LOG_INFO(
            "We have " << unsigned(m_retxParams.retxLeft)
                       << " transmissions left. We were not transmitting confirmed messages.");

        // Reset retransmission parameters
        resetRetransmissionParameters();
    }
}

/////////////////////////
// Getters and Setters //
/////////////////////////

Time
ClassAEndDeviceLorawanMac::GetNextClassTransmissionDelay(Time waitingTime)
{
    NS_LOG_FUNCTION_NOARGS();

    // This is a new packet from APP; it can not be sent until the end of the
    // second receive window (if the second receive window has not closed yet)
    if (!m_retxParams.waitingAck)
    {
        if (!m_closeFirstWindow.IsExpired() || !m_closeSecondWindow.IsExpired() ||
            !m_secondReceiveWindow.IsExpired())
        {
            NS_LOG_WARN(
                "Attempting to send when there are receive windows: Transmission postponed.");
            // Compute the duration of a single symbol for the second receive window data rate
            double tSym = pow(2, GetSfFromDataRate(GetSecondReceiveWindowDataRate())) /
                          GetBandwidthFromDataRate(GetSecondReceiveWindowDataRate());
            // Compute the closing time of the second receive window
            Time endSecondRxWindow = Time(m_secondReceiveWindow.GetTs()) +
                                     Seconds(m_receiveWindowDurationInSymbols * tSym);

            NS_LOG_DEBUG("Duration until endSecondRxWindow for new transmission:"
                         << (endSecondRxWindow - Simulator::Now()).GetSeconds());
            waitingTime = std::max(waitingTime, endSecondRxWindow - Simulator::Now());
        }
    }
    // This is a retransmitted packet, it can not be sent until the end of
    // ACK_TIMEOUT (this timer starts when the second receive window was open)
    else
    {
        double ack_timeout = m_uniformRV->GetValue(1, 3);
        // Compute the duration until ACK_TIMEOUT (It may be a negative number, but it doesn't
        // matter.)
        Time retransmitWaitingTime =
            Time(m_secondReceiveWindow.GetTs()) - Simulator::Now() + Seconds(ack_timeout);

        NS_LOG_DEBUG("ack_timeout:" << ack_timeout << " retransmitWaitingTime:"
                                    << retransmitWaitingTime.GetSeconds());
        waitingTime = std::max(waitingTime, retransmitWaitingTime);
    }

    return waitingTime;
}

uint8_t
ClassAEndDeviceLorawanMac::GetFirstReceiveWindowDataRate()
{
    return m_replyDataRateMatrix.at(m_dataRate).at(m_rx1DrOffset);
}

void
ClassAEndDeviceLorawanMac::SetSecondReceiveWindowDataRate(uint8_t dataRate)
{
    m_secondReceiveWindowDataRate = dataRate;
}

uint8_t
ClassAEndDeviceLorawanMac::GetSecondReceiveWindowDataRate() const
{
    return m_secondReceiveWindowDataRate;
}

void
ClassAEndDeviceLorawanMac::SetSecondReceiveWindowFrequency(double frequencyMHz)
{
    m_secondReceiveWindowFrequency = frequencyMHz;
}

double
ClassAEndDeviceLorawanMac::GetSecondReceiveWindowFrequency() const
{
    return m_secondReceiveWindowFrequency;
}

/////////////////////////
// MAC command methods //
/////////////////////////

void
ClassAEndDeviceLorawanMac::OnRxClassParamSetupReq(Ptr<RxParamSetupReq> rxParamSetupReq)
{
    NS_LOG_FUNCTION(this << rxParamSetupReq);

    bool offsetOk = true;
    bool dataRateOk = true;

    uint8_t rx1DrOffset = rxParamSetupReq->GetRx1DrOffset();
    uint8_t rx2DataRate = rxParamSetupReq->GetRx2DataRate();
    double frequency = rxParamSetupReq->GetFrequency();

    NS_LOG_FUNCTION(this << unsigned(rx1DrOffset) << unsigned(rx2DataRate) << frequency);

    // Check that the desired offset is valid
    if (!(0 <= rx1DrOffset && rx1DrOffset <= 5))
    {
        offsetOk = false;
    }

    // Check that the desired data rate is valid
    if (GetSfFromDataRate(rx2DataRate) == 0 || GetBandwidthFromDataRate(rx2DataRate) == 0)
    {
        dataRateOk = false;
    }

    // For now, don't check for validity of frequency
    m_secondReceiveWindowDataRate = rx2DataRate;
    m_rx1DrOffset = rx1DrOffset;
    m_secondReceiveWindowFrequency = frequency;

    // Craft a RxParamSetupAns as response
    NS_LOG_INFO("Adding RxParamSetupAns reply");
    m_macCommandList.emplace_back(CreateObject<RxParamSetupAns>(offsetOk, dataRateOk, true));
}

} /* namespace lorawan */
} /* namespace ns3 */
