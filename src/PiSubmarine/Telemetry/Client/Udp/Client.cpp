#include "PiSubmarine/Telemetry/Client/Udp/Client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "PiSubmarine/Error/Api/MakeError.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    namespace
    {
        constexpr std::size_t EncodedFieldSize = sizeof(std::uint32_t);

        [[nodiscard]] std::uint32_t ReadUInt32BigEndian(std::span<const std::byte> bytes, std::size_t offset)
        {
            return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) << 24U)
                | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 16U)
                | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 8U)
                | static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3]));
        }

        [[nodiscard]] Error::Api::Result<std::map<Api::ChannelId, std::vector<std::byte>>> ParsePayloads(
            std::span<const std::byte> bytes)
        {
            if (bytes.size() < EncodedFieldSize)
            {
                return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
            }

            std::size_t offset = 0;
            const auto channelCount = ReadUInt32BigEndian(bytes, offset);
            offset += EncodedFieldSize;

            std::map<Api::ChannelId, std::vector<std::byte>> payloads;
            for (std::uint32_t channelIndex = 0; channelIndex < channelCount; ++channelIndex)
            {
                if (bytes.size() - offset < EncodedFieldSize)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                const auto channelLength = ReadUInt32BigEndian(bytes, offset);
                offset += EncodedFieldSize;
                if (bytes.size() - offset < channelLength)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                std::string channelValue;
                channelValue.reserve(channelLength);
                for (std::uint32_t index = 0; index < channelLength; ++index)
                {
                    channelValue.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index])));
                }
                offset += channelLength;

                if (bytes.size() - offset < EncodedFieldSize)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                const auto payloadLength = ReadUInt32BigEndian(bytes, offset);
                offset += EncodedFieldSize;
                if (bytes.size() - offset < payloadLength)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                auto [iterator, inserted] = payloads.emplace(
                    Api::ChannelId{.Value = std::move(channelValue)},
                    std::vector<std::byte>{});
                if (!inserted)
                {
                    return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
                }

                iterator->second.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + payloadLength));
                offset += payloadLength;
            }

            if (offset != bytes.size())
            {
                return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
            }

            return payloads;
        }
    }

    Client::Client(
        Lease::Api::ILeaseIssuer& leaseIssuer,
        ::PiSubmarine::Udp::Api::IReceiver& receiver,
        ::PiSubmarine::Udp::Api::ISender& sender,
        ::PiSubmarine::Udp::Api::Endpoint serverEndpoint,
        const std::chrono::milliseconds acquireRetryInterval,
        const std::chrono::milliseconds subscribeRetryInterval)
        : m_LeaseIssuer(leaseIssuer)
        , m_Receiver(receiver)
        , m_Sender(sender)
        , m_ServerEndpoint(std::move(serverEndpoint))
        , m_AcquireRetryInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(acquireRetryInterval))
        , m_SubscribeRetryInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(subscribeRetryInterval))
        , m_LastError(Error::Api::MakeError(Error::Api::ErrorCondition::CommunicationError))
    {
    }

    Client::~Client()
    {
        if (!m_Lease.has_value())
        {
            return;
        }

        [[maybe_unused]] const auto releaseResult = m_LeaseIssuer.ReleaseLease(m_Lease->Id);
    }

    Error::Api::Result<std::vector<std::byte>> Client::GetRaw(const Api::ChannelId& channel) const
    {
        if (!m_HasCurrentDatagram)
        {
            return std::unexpected(m_LastError.value_or(
                Error::Api::MakeError(Error::Api::ErrorCondition::UnknownError)));
        }

        const auto iterator = m_Payloads.find(channel);
        if (iterator == m_Payloads.end())
        {
            return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
        }

        return iterator->second;
    }

    void Client::Tick(const std::chrono::nanoseconds& uptime, const std::chrono::nanoseconds& deltaTime)
    {
        static_cast<void>(deltaTime);

        if (EnsureLease(uptime))
        {
            RenewLease(uptime);
            TrySubscribe(uptime);
        }

        ReceiveDatagrams();
    }

    Lease::Api::ResourceId Client::MakeTelemetryResourceId()
    {
        return Lease::Api::ResourceId{.Value = "telemetry-main"};
    }

    std::chrono::nanoseconds Client::ComputeRenewInterval(const Lease::Api::Lease& lease)
    {
        const auto halfDuration = lease.Duration / 2;
        const auto boundedDuration = std::max(halfDuration, std::chrono::milliseconds(1));
        return std::chrono::duration_cast<std::chrono::nanoseconds>(boundedDuration);
    }

    bool Client::EnsureLease(const std::chrono::nanoseconds& uptime)
    {
        if (m_Lease.has_value())
        {
            return true;
        }

        if (uptime < m_NextAcquireAttempt)
        {
            return false;
        }

        const auto acquireResult = m_LeaseIssuer.AcquireLease(Lease::Api::LeaseRequest{
            .Resource = MakeTelemetryResourceId()});
        if (!acquireResult.has_value())
        {
            m_LastError = acquireResult.error();
            m_NextAcquireAttempt = uptime + m_AcquireRetryInterval;
            return false;
        }

        m_Lease = *acquireResult;
        m_LastError.reset();
        m_NextRenewTime = uptime + ComputeRenewInterval(*m_Lease);
        m_NextSubscribeAttempt = uptime;
        m_IsSubscriptionPending = true;
        return true;
    }

    void Client::RenewLease(const std::chrono::nanoseconds& uptime)
    {
        if (!m_Lease.has_value() || uptime < m_NextRenewTime)
        {
            return;
        }

        const auto renewResult = m_LeaseIssuer.RenewLease(m_Lease->Id);
        if (!renewResult.has_value())
        {
            m_LastError = renewResult.error();
            m_Lease.reset();
            m_NextAcquireAttempt = uptime + m_AcquireRetryInterval;
            m_IsSubscriptionPending = false;
            return;
        }

        m_Lease = *renewResult;
        m_LastError.reset();
        m_NextRenewTime = uptime + ComputeRenewInterval(*m_Lease);
        m_NextSubscribeAttempt = uptime;
        m_IsSubscriptionPending = true;
    }

    void Client::TrySubscribe(const std::chrono::nanoseconds& uptime)
    {
        if (!m_Lease.has_value() || !m_IsSubscriptionPending || uptime < m_NextSubscribeAttempt)
        {
            return;
        }

        std::vector<std::byte> payload;
        payload.reserve(m_Lease->Id.Value.size());
        for (const char character : m_Lease->Id.Value)
        {
            payload.push_back(static_cast<std::byte>(character));
        }

        const auto sendResult = m_Sender.Send(::PiSubmarine::Udp::Api::Datagram{
            .Peer = m_ServerEndpoint,
            .Payload = std::move(payload)});

        if (!sendResult.has_value())
        {
            m_LastError = sendResult.error();
            m_NextSubscribeAttempt = uptime + m_SubscribeRetryInterval;
            return;
        }

        m_LastError.reset();
        m_IsSubscriptionPending = false;
    }

    void Client::ReceiveDatagrams()
    {
        while (true)
        {
            const auto receiveResult = m_Receiver.TryReceive();
            if (!receiveResult.has_value() || !receiveResult->has_value())
            {
                return;
            }

            const auto payloadsResult = ParsePayloads(receiveResult->value().Payload);
            if (!payloadsResult.has_value())
            {
                m_Payloads.clear();
                m_HasCurrentDatagram = false;
                m_LastError = payloadsResult.error();
                continue;
            }

            m_Payloads = std::move(*payloadsResult);
            m_HasCurrentDatagram = true;
            m_LastError.reset();
        }
    }
}
