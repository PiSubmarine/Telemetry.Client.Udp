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

        [[nodiscard]] Error::Api::Error MakeContractError()
        {
            return Error::Api::MakeError(Error::Api::ErrorCondition::ContractError);
        }

        [[nodiscard]] std::uint32_t ReadUInt32BigEndian(std::span<const std::byte> bytes, std::size_t offset)
        {
            return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) << 24U)
                | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 16U)
                | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 8U)
                | static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3]));
        }

        void AppendUInt32BigEndian(std::vector<std::byte>& bytes, const std::uint32_t value)
        {
            bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
            bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
            bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
            bytes.push_back(static_cast<std::byte>(value & 0xFFU));
        }

        [[nodiscard]] std::vector<std::byte> EncodeString(const std::string& value)
        {
            std::vector<std::byte> bytes;
            bytes.reserve(value.size());
            for (const char character : value)
            {
                bytes.push_back(static_cast<std::byte>(character));
            }

            return bytes;
        }

        [[nodiscard]] Security::Aead::Api::Key MakeKey(const Lease::Api::LeaseSecret& leaseSecret)
        {
            return Security::Aead::Api::Key{.Value = leaseSecret.Value};
        }

        [[nodiscard]] Security::Aead::Api::AssociatedData MakeAssociatedData(const Lease::Api::LeaseId& leaseId)
        {
            return Security::Aead::Api::AssociatedData{.Value = EncodeString(leaseId.Value)};
        }

        struct Packet
        {
            Lease::Api::LeaseId LeaseId;
            Security::Api::Nonce Nonce;
            Security::Aead::Api::Ciphertext Ciphertext;
        };

        [[nodiscard]] Error::Api::Result<Packet> ParsePacket(std::span<const std::byte> bytes)
        {
            if (bytes.size() < EncodedFieldSize * 2)
            {
                return std::unexpected(MakeContractError());
            }

            std::size_t offset = 0;
            const auto leaseIdLength = ReadUInt32BigEndian(bytes, offset);
            offset += EncodedFieldSize;
            if (bytes.size() - offset < leaseIdLength + EncodedFieldSize)
            {
                return std::unexpected(MakeContractError());
            }

            Lease::Api::LeaseId leaseId;
            leaseId.Value.reserve(leaseIdLength);
            for (std::uint32_t index = 0; index < leaseIdLength; ++index)
            {
                leaseId.Value.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index])));
            }
            offset += leaseIdLength;

            const auto nonceLength = ReadUInt32BigEndian(bytes, offset);
            offset += EncodedFieldSize;
            if (bytes.size() - offset < nonceLength)
            {
                return std::unexpected(MakeContractError());
            }

            Security::Api::Nonce nonce;
            nonce.Value.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + nonceLength));
            offset += nonceLength;

            Security::Aead::Api::Ciphertext ciphertext;
            ciphertext.Value.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.end());

            return Packet{
                .LeaseId = std::move(leaseId),
                .Nonce = std::move(nonce),
                .Ciphertext = std::move(ciphertext)};
        }

        [[nodiscard]] std::vector<std::byte> BuildPacket(
            const Lease::Api::LeaseId& leaseId,
            const Security::Api::Nonce& nonce,
            const Security::Aead::Api::Ciphertext& ciphertext)
        {
            std::vector<std::byte> bytes;
            bytes.reserve(EncodedFieldSize * 2 + leaseId.Value.size() + nonce.Value.size() + ciphertext.Value.size());

            AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(leaseId.Value.size()));
            const auto leaseIdBytes = EncodeString(leaseId.Value);
            bytes.insert(bytes.end(), leaseIdBytes.begin(), leaseIdBytes.end());

            AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(nonce.Value.size()));
            bytes.insert(bytes.end(), nonce.Value.begin(), nonce.Value.end());
            bytes.insert(bytes.end(), ciphertext.Value.begin(), ciphertext.Value.end());

            return bytes;
        }

        [[nodiscard]] Error::Api::Result<std::map<Api::ChannelId, std::vector<std::byte>>> ParsePayloads(
            std::span<const std::byte> bytes)
        {
            if (bytes.size() < EncodedFieldSize)
            {
                return std::unexpected(MakeContractError());
            }

            std::size_t offset = 0;
            const auto channelCount = ReadUInt32BigEndian(bytes, offset);
            offset += EncodedFieldSize;

            std::map<Api::ChannelId, std::vector<std::byte>> payloads;
            for (std::uint32_t channelIndex = 0; channelIndex < channelCount; ++channelIndex)
            {
                if (bytes.size() - offset < EncodedFieldSize)
                {
                    return std::unexpected(MakeContractError());
                }

                const auto channelLength = ReadUInt32BigEndian(bytes, offset);
                offset += EncodedFieldSize;
                if (bytes.size() - offset < channelLength)
                {
                    return std::unexpected(MakeContractError());
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
                    return std::unexpected(MakeContractError());
                }

                const auto payloadLength = ReadUInt32BigEndian(bytes, offset);
                offset += EncodedFieldSize;
                if (bytes.size() - offset < payloadLength)
                {
                    return std::unexpected(MakeContractError());
                }

                auto [iterator, inserted] = payloads.emplace(
                    Api::ChannelId{.Value = std::move(channelValue)},
                    std::vector<std::byte>{});
                if (!inserted)
                {
                    return std::unexpected(MakeContractError());
                }

                iterator->second.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + payloadLength));
                offset += payloadLength;
            }

            if (offset != bytes.size())
            {
                return std::unexpected(MakeContractError());
            }

            return payloads;
        }
    }

    Client::Client(
        Lease::Api::ILeaseIssuer& leaseIssuer,
        const ::PiSubmarine::Security::Aead::Api::IProvider& aeadProvider,
        ::PiSubmarine::Security::Api::INonceProvider& nonceProvider,
        ::PiSubmarine::Udp::Api::IReceiver& receiver,
        ::PiSubmarine::Udp::Api::ISender& sender,
        ::PiSubmarine::Udp::Api::Endpoint serverEndpoint,
        const std::chrono::milliseconds acquireRetryInterval,
        const std::chrono::milliseconds subscribeRetryInterval)
        : m_LeaseIssuer(leaseIssuer)
        , m_AeadProvider(aeadProvider)
        , m_NonceProvider(nonceProvider)
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

    std::optional<Lease::Api::Lease> Client::GetLease() const noexcept
    {
        if (!m_Lease.has_value() || !m_LeaseSecret.has_value())
        {
            return std::nullopt;
        }

        return m_Lease;
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
            if (acquireResult.error().Condition == Error::Api::ErrorCondition::NotReady)
            {
                return false;
            }

            m_LastError = acquireResult.error();
            m_NextAcquireAttempt = uptime + m_AcquireRetryInterval;
            return false;
        }

        m_Lease = acquireResult->GrantedLease;
        m_LeaseSecret = acquireResult->Secret;
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
            if (renewResult.error().Condition == Error::Api::ErrorCondition::NotReady)
            {
                return;
            }

            m_LastError = renewResult.error();
            m_Lease.reset();
            m_LeaseSecret.reset();
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
        if (!m_Lease.has_value() || !m_LeaseSecret.has_value() || !m_IsSubscriptionPending || uptime < m_NextSubscribeAttempt)
        {
            return;
        }

        const auto nonceResult = m_NonceProvider.Next();
        if (!nonceResult.has_value())
        {
            m_LastError = nonceResult.error();
            m_NextSubscribeAttempt = uptime + m_SubscribeRetryInterval;
            return;
        }

        const auto ciphertextResult = m_AeadProvider.Seal(
            MakeKey(*m_LeaseSecret),
            *nonceResult,
            Security::Aead::Api::Plaintext{},
            MakeAssociatedData(m_Lease->Id));
        if (!ciphertextResult.has_value())
        {
            m_LastError = ciphertextResult.error();
            m_NextSubscribeAttempt = uptime + m_SubscribeRetryInterval;
            return;
        }

        const auto sendResult = m_Sender.Send(::PiSubmarine::Udp::Api::Datagram{
            .Peer = m_ServerEndpoint,
            .Payload = BuildPacket(m_Lease->Id, *nonceResult, *ciphertextResult)});

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

            if (!m_Lease.has_value() || !m_LeaseSecret.has_value())
            {
                continue;
            }

            const auto packetResult = ParsePacket(receiveResult->value().Payload);
            if (!packetResult.has_value())
            {
                m_Payloads.clear();
                m_HasCurrentDatagram = false;
                m_LastError = packetResult.error();
                continue;
            }

            if (packetResult->LeaseId != m_Lease->Id)
            {
                m_Payloads.clear();
                m_HasCurrentDatagram = false;
                m_LastError = MakeContractError();
                continue;
            }

            const auto plaintextResult = m_AeadProvider.Open(
                MakeKey(*m_LeaseSecret),
                packetResult->Nonce,
                packetResult->Ciphertext,
                MakeAssociatedData(packetResult->LeaseId));
            if (!plaintextResult.has_value())
            {
                m_Payloads.clear();
                m_HasCurrentDatagram = false;
                m_LastError = plaintextResult.error();
                continue;
            }

            const auto payloadsResult = ParsePayloads(plaintextResult->Value);
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
