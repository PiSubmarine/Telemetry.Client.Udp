#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "PiSubmarine/Error/Api/ErrorCondition.h"
#include "PiSubmarine/Error/Api/MakeError.h"
#include "PiSubmarine/Lease/Api/ILeaseIssuerMock.h"
#include "PiSubmarine/Security/Api/INonceProviderMock.h"
#include "PiSubmarine/Security/Aead/Api/IProviderMock.h"
#include "PiSubmarine/Telemetry/Api/IRawCacheMock.h"
#include "PiSubmarine/Telemetry/Client/Udp/Client.h"
#include "PiSubmarine/Telemetry/Client/Udp/Source.h"
#include "PiSubmarine/Udp/Api/IReceiverMock.h"
#include "PiSubmarine/Udp/Api/ISenderMock.h"

namespace PiSubmarine::Telemetry::Client::Udp
{
    namespace
    {
        using ::testing::_;
        using ::testing::Return;
        using ::testing::StrictMock;

        constexpr std::size_t EncodedFieldSize = sizeof(std::uint32_t);

        void AppendUInt32BigEndian(std::vector<std::byte>& bytes, const std::uint32_t value)
        {
            bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
            bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
            bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
            bytes.push_back(static_cast<std::byte>(value & 0xFFU));
        }

        [[nodiscard]] std::vector<std::byte> EncodeDatagram(
            const std::initializer_list<std::pair<std::string_view, std::vector<std::byte>>> channels)
        {
            std::vector<std::byte> bytes;
            AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(channels.size()));
            for (const auto& [channel, payload] : channels)
            {
                AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(channel.size()));
                for (const char character : channel)
                {
                    bytes.push_back(static_cast<std::byte>(character));
                }

                AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(payload.size()));
                bytes.insert(bytes.end(), payload.begin(), payload.end());
            }

            return bytes;
        }

        [[nodiscard]] std::vector<std::byte> EncodePacket(
            std::string_view leaseId,
            const std::vector<std::byte>& nonce,
            const std::vector<std::byte>& ciphertext)
        {
            std::vector<std::byte> bytes;
            bytes.reserve(EncodedFieldSize * 2 + leaseId.size() + nonce.size() + ciphertext.size());
            AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(leaseId.size()));
            for (const char character : leaseId)
            {
                bytes.push_back(static_cast<std::byte>(character));
            }

            AppendUInt32BigEndian(bytes, static_cast<std::uint32_t>(nonce.size()));
            bytes.insert(bytes.end(), nonce.begin(), nonce.end());
            bytes.insert(bytes.end(), ciphertext.begin(), ciphertext.end());
            return bytes;
        }

        [[nodiscard]] Lease::Api::LeaseGrant MakeLeaseGrant()
        {
            return Lease::Api::LeaseGrant{
                .GrantedLease = Lease::Api::Lease{
                    .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                    .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                    .Duration = std::chrono::milliseconds(3000)},
                .Secret = Lease::Api::LeaseSecret{
                    .Value = {
                        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                        std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
                        std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B},
                        std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
                        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                        std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B},
                        std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}}}};
        }

        [[nodiscard]] Security::Aead::Api::Key MakeKey()
        {
            return Security::Aead::Api::Key{.Value = MakeLeaseGrant().Secret.Value};
        }

        [[nodiscard]] Security::Aead::Api::AssociatedData MakeAssociatedData()
        {
            return Security::Aead::Api::AssociatedData{
                .Value = {
                    std::byte{'l'}, std::byte{'e'}, std::byte{'a'},
                    std::byte{'s'}, std::byte{'e'}, std::byte{'-'}, std::byte{'1'}}};
        }
    }

    TEST(ClientTest, AcquiresLeaseAndSendsAuthenticatedSubscriptionOnFirstTick)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(Lease::Api::LeaseRequest{
                        .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"}}))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0xAA}, std::byte{0xBB}}})));
        EXPECT_CALL(aeadProvider, Seal(
                        MakeKey(),
                        Security::Api::Nonce{.Value = {std::byte{0xAA}, std::byte{0xBB}}},
                        Security::Aead::Api::Plaintext{},
                        MakeAssociatedData()))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0xCC}, std::byte{0xDD}}})));
        EXPECT_CALL(sender, Send(::testing::Truly([](const ::PiSubmarine::Udp::Api::Datagram& datagram)
            {
                return datagram.Peer == ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000}
                    && datagram.Payload == EncodePacket(
                        "lease-1",
                        {std::byte{0xAA}, std::byte{0xBB}},
                        {std::byte{0xCC}, std::byte{0xDD}});
            })))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto rawPayload = client.GetRaw(Api::ChannelId{.Value = "battery.main"});
        ASSERT_FALSE(rawPayload.has_value());
        EXPECT_EQ(rawPayload.error().Condition, Error::Api::ErrorCondition::UnknownError);
    }

    TEST(ClientTest, RenewsLeaseAndResubscribesWhenLeaseIsNearExpiry)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x01}}})))
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x02}}})));
        EXPECT_CALL(aeadProvider, Seal(_, _, Security::Aead::Api::Plaintext{}, _))
            .Times(2)
            .WillRepeatedly(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0xF0}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .Times(2)
            .WillRepeatedly(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, RenewLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<Lease::Api::Lease>(Lease::Api::Lease{
                .Id = Lease::Api::LeaseId{.Value = "lease-1"},
                .Resource = Lease::Api::ResourceId{.Value = "telemetry-main"},
                .Duration = std::chrono::milliseconds(3000)})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));
        client.Tick(std::chrono::milliseconds(2500), std::chrono::milliseconds(10));
    }

    TEST(ClientTest, RetriesAcquireWithoutBackoffWhenLeaseIssuerReturnsNotReady)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::NotReady))))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x01}}})));
        EXPECT_CALL(aeadProvider, Seal(_, _, Security::Aead::Api::Plaintext{}, _))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0xF0}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .Times(2)
            .WillRepeatedly(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));
        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));
    }

    TEST(ClientTest, KeepsCurrentLeaseWhenRenewReturnsNotReady)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x01}}})));
        EXPECT_CALL(aeadProvider, Seal(_, _, Security::Aead::Api::Plaintext{}, _))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0xF0}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .Times(2)
            .WillRepeatedly(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(leaseIssuer, RenewLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::NotReady))));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));
        client.Tick(std::chrono::milliseconds(2500), std::chrono::milliseconds(10));
    }

    TEST(ClientTest, UpdatesRawPayloadCacheFromAuthenticatedDatagram)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        const auto telemetryPayload = EncodeDatagram({
            {"battery.main", {std::byte{0xAA}, std::byte{0xBB}}},
            {"motor.front-left", {std::byte{0xCC}}}});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x01}}})));
        EXPECT_CALL(aeadProvider, Seal(_, _, Security::Aead::Api::Plaintext{}, _))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0x99}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = EncodePacket("lease-1", {std::byte{0x10}}, {std::byte{0x20}, std::byte{0x21}})})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(aeadProvider, Open(
                        MakeKey(),
                        Security::Api::Nonce{.Value = {std::byte{0x10}}},
                        Security::Aead::Api::Ciphertext{.Value = {std::byte{0x20}, std::byte{0x21}}},
                        MakeAssociatedData()))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Plaintext>(
                Security::Aead::Api::Plaintext{.Value = telemetryPayload})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto batteryPayload = client.GetRaw(Api::ChannelId{.Value = "battery.main"});
        ASSERT_TRUE(batteryPayload.has_value());
        EXPECT_EQ(*batteryPayload, (std::vector<std::byte>{std::byte{0xAA}, std::byte{0xBB}}));

        const auto motorPayload = client.GetRaw(Api::ChannelId{.Value = "motor.front-left"});
        ASSERT_TRUE(motorPayload.has_value());
        EXPECT_EQ(*motorPayload, (std::vector<std::byte>{std::byte{0xCC}}));
    }

    TEST(ClientTest, ReportsContractErrorForMissingChannelInLatestDatagram)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x01}}})));
        EXPECT_CALL(aeadProvider, Seal(_, _, Security::Aead::Api::Plaintext{}, _))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0x99}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = EncodePacket("lease-1", {std::byte{0x10}}, {std::byte{0x20}})})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(aeadProvider, Open(_, _, _, _))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Plaintext>(
                Security::Aead::Api::Plaintext{.Value = EncodeDatagram({{"battery.main", {std::byte{0x01}}}})})));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto payload = client.GetRaw(Api::ChannelId{.Value = "motor.front-left"});
        ASSERT_FALSE(payload.has_value());
        EXPECT_EQ(payload.error().Condition, Error::Api::ErrorCondition::ContractError);
    }

    TEST(ClientTest, AuthenticationFailureClearsCachedPayloads)
    {
        StrictMock<Lease::Api::ILeaseIssuerMock> leaseIssuer;
        StrictMock<Security::Aead::Api::IProviderMock> aeadProvider;
        StrictMock<Security::Api::INonceProviderMock> nonceProvider;
        StrictMock<::PiSubmarine::Udp::Api::IReceiverMock> receiver;
        StrictMock<::PiSubmarine::Udp::Api::ISenderMock> sender;

        Client client(
            leaseIssuer,
            aeadProvider,
            nonceProvider,
            receiver,
            sender,
            ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000});

        EXPECT_CALL(leaseIssuer, AcquireLease(_))
            .WillOnce(Return(Error::Api::Result<Lease::Api::LeaseGrant>(MakeLeaseGrant())));
        EXPECT_CALL(nonceProvider, Next())
            .WillOnce(Return(Error::Api::Result<Security::Api::Nonce>(
                Security::Api::Nonce{.Value = {std::byte{0x01}}})));
        EXPECT_CALL(aeadProvider, Seal(_, _, Security::Aead::Api::Plaintext{}, _))
            .WillOnce(Return(Error::Api::Result<Security::Aead::Api::Ciphertext>(
                Security::Aead::Api::Ciphertext{.Value = {std::byte{0x99}}})));
        EXPECT_CALL(sender, Send(_))
            .WillOnce(Return(Error::Api::Result<void>{}));
        EXPECT_CALL(receiver, TryReceive())
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                ::PiSubmarine::Udp::Api::Datagram{
                    .Peer = ::PiSubmarine::Udp::Api::Endpoint{"127.0.0.1", 9000},
                    .Payload = EncodePacket("lease-1", {std::byte{0x10}}, {std::byte{0x20}})})))
            .WillOnce(Return(Error::Api::Result<std::optional<::PiSubmarine::Udp::Api::Datagram>>(
                std::optional<::PiSubmarine::Udp::Api::Datagram>{std::nullopt})));
        EXPECT_CALL(aeadProvider, Open(_, _, _, _))
            .WillOnce(Return(std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::CommunicationError))));
        EXPECT_CALL(leaseIssuer, ReleaseLease(Lease::Api::LeaseId{.Value = "lease-1"}))
            .WillOnce(Return(Error::Api::Result<void>{}));

        client.Tick(std::chrono::seconds(1), std::chrono::milliseconds(10));

        const auto payload = client.GetRaw(Api::ChannelId{.Value = "battery.main"});
        ASSERT_FALSE(payload.has_value());
        EXPECT_EQ(payload.error().Condition, Error::Api::ErrorCondition::CommunicationError);
    }

    TEST(SourceTest, DelegatesToRawCacheForConfiguredChannel)
    {
        StrictMock<Api::IRawCacheMock> rawCache;
        Source source(rawCache, Api::ChannelId{.Value = "battery.main"});
        const std::vector<std::byte> expectedPayload{std::byte{0x11}, std::byte{0x22}};

        EXPECT_CALL(rawCache, GetRaw(Api::ChannelId{.Value = "battery.main"}))
            .WillOnce(Return(Error::Api::Result<std::vector<std::byte>>(expectedPayload)));

        const auto payload = source.GetRaw();

        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(*payload, expectedPayload);
    }
}
