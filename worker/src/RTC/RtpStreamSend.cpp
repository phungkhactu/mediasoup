#define MS_CLASS "RTC::RtpStreamSend"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/RtpStreamSend.hpp"
#include "Logger.hpp"
#include "Utils.hpp"
#include "RTC/SeqManager.hpp"

namespace RTC
{
	/* Static. */

	thread_local static Utils::ObjectPool<RtpStreamSend::StorageItem> StorageItemPool;

	// 17: 16 bit mask + the initial sequence number.
	static constexpr size_t MaxRequestedPackets{ 17 };
	thread_local static std::vector<RTC::RtpStreamSend::StorageItem*> RetransmissionContainer(
	  MaxRequestedPackets + 1);
	static constexpr uint32_t MinRetransmissionDelay{ 200 };
	// Don't retransmit packets older than this (ms).
	static constexpr uint32_t MaxRetransmissionDelay{ 2000 };
	static constexpr uint32_t DefaultRtt{ 100 };
	static constexpr size_t SeqRange{ 65536 };
	static constexpr uint16_t MaxSeq = std::numeric_limits<uint16_t>::max();

	static void resetStorageItem(RTC::RtpStreamSend::StorageItem* storageItem)
	{
		MS_TRACE();

		MS_ASSERT(storageItem, "storageItem cannot be nullptr");

		storageItem->clonedPacket   = nullptr;
		storageItem->originalPacket = nullptr;
		storageItem->resentAtMs     = 0;
		storageItem->sentTimes      = 0;
		storageItem->rtxEncoded     = false;
	}

	RtpStreamSend::StorageItem* RtpStreamSend::StorageItemBuffer::Get(uint16_t seq)
	{
		auto idx{ static_cast<uint16_t>((seq - this->startSeq) % MaxSeq) };

		if (this->buffer.empty() || idx >= static_cast<uint16_t>(this->buffer.size()))
			return nullptr;

		return this->buffer.at(idx);
	}

	bool RtpStreamSend::StorageItemBuffer::Insert(uint16_t seq, StorageItem* storageItem)
	{
		if (this->buffer.empty())
		{
			this->startSeq = seq;
			this->buffer.push_back(storageItem);

			return true;
		}

		auto idx{ static_cast<uint16_t>((seq - this->startSeq) % MaxSeq) };

		if (idx < static_cast<uint16_t>(this->buffer.size()))
		{
			this->buffer[idx] = storageItem;

			return true;
		}

		// Calculate how many elements would it be necessary to add when pushing new item to the back of
		// the deque.
		auto addToBack{ static_cast<uint16_t>((seq - (this->startSeq + this->buffer.size() - 1))) %
			              MaxSeq };
		// Calculate how many elements would it be necessary to add when pushing new item to the front
		// of the deque.
		auto addToFront{ static_cast<uint16_t>((this->startSeq - seq) % MaxSeq) };

		// Select the side of deque where fewer elements need to be added, while preferring the end.
		if (addToBack <= addToFront)
		{
			// Packets can arrive out of order, add blank slots.
			for (uint16_t i{ 1 }; i < addToBack; ++i)
				this->buffer.push_back(nullptr);

			this->buffer.push_back(storageItem);
		}
		else
		{
			// Packets can arrive out of order, add blank slots.
			for (uint16_t i{ 1 }; i < addToFront; ++i)
				this->buffer.push_front(nullptr);

			this->buffer.push_front(storageItem);
			this->startSeq = seq;
		}

		return true;
	}

	bool RtpStreamSend::StorageItemBuffer::Remove(uint16_t seq)
	{
		if (this->buffer.empty())
			return false;

		auto idx{ static_cast<uint16_t>((seq - this->startSeq) % MaxSeq) };

		this->buffer[idx] = nullptr;

		// If we have erased the first element, remove all `nullptr` elements from the beginning of the buffer.
		if (idx == 0)
		{
			while (!this->buffer.front())
			{
				this->buffer.pop_front();
				this->startSeq++;
			}
		}
		// If we have erased the last element, remove all `nullptr` elements from the end of the buffer.
		else if (idx == static_cast<uint16_t>(this->buffer.size() - 1))
		{
			while (!this->buffer.back())
				this->buffer.pop_back();
		}

		return true;
	}

	void RtpStreamSend::StorageItemBuffer::Clear()
	{
		for (auto* storageItem : this->buffer)
		{
			if (!storageItem)
				continue;

			// Reset (free RTP packet) the storage item.
			resetStorageItem(storageItem);
			// Return into the pool.
			StorageItemPool.Return(storageItem);
		}

		this->buffer.clear();
	}

	RtpStreamSend::StorageItemBuffer::~StorageItemBuffer()
	{
		Clear();
	}

	/* Instance methods. */

	RtpStreamSend::RtpStreamSend(
	  RTC::RtpStreamSend::Listener* listener, RTC::RtpStream::Params& params, std::string& mid, bool useNack)
	  : RTC::RtpStream::RtpStream(listener, params, 10), mid(mid), useNack(useNack),
	    retransmissionBufferSize(MaxRetransmissionDelay)
	{
		MS_TRACE();
	}

	RtpStreamSend::~RtpStreamSend()
	{
		MS_TRACE();

		// Clear the RTP buffer.
		ClearBuffer();
	}

	void RtpStreamSend::FillJsonStats(json& jsonObject)
	{
		MS_TRACE();

		uint64_t nowMs = DepLibUV::GetTimeMs();

		RTC::RtpStream::FillJsonStats(jsonObject);

		jsonObject["type"]        = "outbound-rtp";
		jsonObject["packetCount"] = this->transmissionCounter.GetPacketCount();
		jsonObject["byteCount"]   = this->transmissionCounter.GetBytes();
		jsonObject["bitrate"]     = this->transmissionCounter.GetBitrate(nowMs);
	}

	void RtpStreamSend::SetRtx(uint8_t payloadType, uint32_t ssrc)
	{
		MS_TRACE();

		RTC::RtpStream::SetRtx(payloadType, ssrc);

		this->rtxSeq = Utils::Crypto::GetRandomUInt(0u, 0xFFFF);
	}

	bool RtpStreamSend::ReceivePacket(RTC::RtpPacket* packet, RTC::RtpPacket::SharedPtr* clonedPacket)
	{
		MS_TRACE();

		// Call the parent method.
		if (!RtpStream::ReceiveStreamPacket(packet))
			return false;

		// If buffer is present, store the packet into the buffer.
		if (this->useNack)
			StorePacket(packet, clonedPacket);

		// Increase transmission counter.
		this->transmissionCounter.Update(packet);

		return true;
	}

	void RtpStreamSend::ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket)
	{
		MS_TRACE();

		this->nackCount++;

		for (auto it = nackPacket->Begin(); it != nackPacket->End(); ++it)
		{
			RTC::RTCP::FeedbackRtpNackItem* item = *it;

			this->nackPacketCount += item->CountRequestedPackets();

			FillRetransmissionContainer(item->GetPacketId(), item->GetLostPacketBitmask());

			for (auto* storageItem : RetransmissionContainer)
			{
				if (!storageItem)
					break;

				// Note that this is an already RTX encoded packet if RTX is used
				// (FillRetransmissionContainer() did it).
				auto packet = storageItem->clonedPacket;

				// Retransmit the packet.
				static_cast<RTC::RtpStreamSend::Listener*>(this->listener)
				  ->OnRtpStreamRetransmitRtpPacket(this, packet.get());

				// Mark the packet as retransmitted.
				RTC::RtpStream::PacketRetransmitted(packet.get());

				// Mark the packet as repaired (only if this is the first retransmission).
				if (storageItem->sentTimes == 1)
					RTC::RtpStream::PacketRepaired(packet.get());
			}
		}
	}

	void RtpStreamSend::ReceiveKeyFrameRequest(RTC::RTCP::FeedbackPs::MessageType messageType)
	{
		MS_TRACE();

		switch (messageType)
		{
			case RTC::RTCP::FeedbackPs::MessageType::PLI:
				this->pliCount++;
				break;

			case RTC::RTCP::FeedbackPs::MessageType::FIR:
				this->firCount++;
				break;

			default:;
		}
	}

	void RtpStreamSend::ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();

		/* Calculate RTT. */

		// Get the NTP representation of the current timestamp.
		uint64_t nowMs = DepLibUV::GetTimeMs();
		auto ntp       = Utils::Time::TimeMs2Ntp(nowMs);

		// Get the compact NTP representation of the current timestamp.
		uint32_t compactNtp = (ntp.seconds & 0x0000FFFF) << 16;

		compactNtp |= (ntp.fractions & 0xFFFF0000) >> 16;

		uint32_t lastSr = report->GetLastSenderReport();
		uint32_t dlsr   = report->GetDelaySinceLastSenderReport();

		// RTT in 1/2^16 second fractions.
		uint32_t rtt{ 0 };

		// If no Sender Report was received by the remote endpoint yet, ignore lastSr
		// and dlsr values in the Receiver Report.
		if (lastSr && dlsr && (compactNtp > dlsr + lastSr))
			rtt = compactNtp - dlsr - lastSr;

		// RTT in milliseconds.
		this->rtt = static_cast<float>(rtt >> 16) * 1000;
		this->rtt += (static_cast<float>(rtt & 0x0000FFFF) / 65536) * 1000;

		if (this->rtt > 0.0f)
		{
			this->hasRtt = true;
		}

		// Smoothly change retransmission buffer size towards RTT + 100ms, but not more than
		// `MaxRetransmissionDelay`.
		auto newRetransmissionBufferSize = static_cast<uint32_t>(this->rtt + 100.0);
		auto avgRetransmissionBufferSize =
		  (this->retransmissionBufferSize * 7 + newRetransmissionBufferSize) / 8;
		this->retransmissionBufferSize = std::max(
		  std::min(avgRetransmissionBufferSize, MaxRetransmissionDelay), MinRetransmissionDelay);

		this->packetsLost  = report->GetTotalLost();
		this->fractionLost = report->GetFractionLost();

		// Update the score with the received RR.
		UpdateScore(report);
	}

	RTC::RTCP::SenderReport* RtpStreamSend::GetRtcpSenderReport(uint64_t nowMs)
	{
		MS_TRACE();

		if (this->transmissionCounter.GetPacketCount() == 0u)
			return nullptr;

		auto ntp     = Utils::Time::TimeMs2Ntp(nowMs);
		auto* report = new RTC::RTCP::SenderReport();

		// Calculate TS difference between now and maxPacketMs.
		auto diffMs = nowMs - this->maxPacketMs;
		auto diffTs = diffMs * GetClockRate() / 1000;

		report->SetSsrc(GetSsrc());
		report->SetPacketCount(this->transmissionCounter.GetPacketCount());
		report->SetOctetCount(this->transmissionCounter.GetBytes());
		report->SetNtpSec(ntp.seconds);
		report->SetNtpFrac(ntp.fractions);
		report->SetRtpTs(this->maxPacketTs + diffTs);

		// Update info about last Sender Report.
		this->lastSenderReportNtpMs = nowMs;
		this->lastSenderReporTs     = this->maxPacketTs + diffTs;

		return report;
	}

	RTC::RTCP::SdesChunk* RtpStreamSend::GetRtcpSdesChunk()
	{
		MS_TRACE();

		const auto& cname = GetCname();
		auto* sdesChunk   = new RTC::RTCP::SdesChunk(GetSsrc());
		auto* sdesItem =
		  new RTC::RTCP::SdesItem(RTC::RTCP::SdesItem::Type::CNAME, cname.size(), cname.c_str());

		sdesChunk->AddItem(sdesItem);

		return sdesChunk;
	}

	void RtpStreamSend::Pause()
	{
		MS_TRACE();

		ClearBuffer();
	}

	void RtpStreamSend::Resume()
	{
		MS_TRACE();
	}

	uint32_t RtpStreamSend::GetBitrate(
	  uint64_t /*nowMs*/, uint8_t /*spatialLayer*/, uint8_t /*temporalLayer*/)
	{
		MS_TRACE();

		MS_ABORT("invalid method call");
	}

	uint32_t RtpStreamSend::GetSpatialLayerBitrate(uint64_t /*nowMs*/, uint8_t /*spatialLayer*/)
	{
		MS_TRACE();

		MS_ABORT("invalid method call");
	}

	uint32_t RtpStreamSend::GetLayerBitrate(
	  uint64_t /*nowMs*/, uint8_t /*spatialLayer*/, uint8_t /*temporalLayer*/)
	{
		MS_TRACE();

		MS_ABORT("invalid method call");
	}

	void RtpStreamSend::StorePacket(RTC::RtpPacket* packet, RTC::RtpPacket::SharedPtr* clonedPacket)
	{
		MS_TRACE();

		if (packet->GetSize() > RTC::MtuSize)
		{
			MS_WARN_TAG(
			  rtp,
			  "packet too big [ssrc:%" PRIu32 ", seq:%" PRIu16 ", size:%zu]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetSize());

			return;
		}

		auto seq          = packet->GetSequenceNumber();
		auto* storageItem = this->storageItemBuffer.Get(seq);

		// The buffer item is already used. Check whether we should replace its
		// storage with the new packet or just ignore it (if duplicated packet).
		if (storageItem)
		{
			auto storedPacket = storageItem->originalPacket;

			if (packet->GetTimestamp() == storedPacket->GetTimestamp())
				return;

			// Reset the storage item.
			resetStorageItem(storageItem);
		}
		// Allocate new buffer item.
		else
		{
			// Allocate a new storage item.
			storageItem = StorageItemPool.Allocate();
			// Memory is not initialized in any way, initialize with default values it to make sure
			// contents is correct.
			*storageItem = StorageItem{};
			MS_ASSERT(this->storageItemBuffer.Insert(seq, storageItem), "sequence number must be empty");

			// Set the beginning of the used buffer.
			if (this->firstPacket)
			{
				this->firstPacket    = false;
				this->bufferStartSeq = seq;
			}
			// Otherwise, try to clean up storage items with packets older than `MaxRetransmissionDelay`.
			else
			{
				uint32_t packetTs{ packet->GetTimestamp() };
				uint32_t clockRate{ this->params.clockRate };

				// Go through all buffer items starting with `this->bufferStartSeq` and free all storage
				// items that contain packets older than `MaxRetransmissionDelay`.
				for (uint32_t i{ 0 }; i < SeqRange; ++i)
				{
					auto* checkedStorageItem = this->storageItemBuffer.Get(this->bufferStartSeq);

					// Packets can arrive out of order, in which case we'll miss some storage items.
					if (checkedStorageItem)
					{
						// This is the storage item we have just inserted, no need to go further.
						if (!checkedStorageItem->originalPacket)
							break;

						uint32_t checkedPacketTs{ checkedStorageItem->originalPacket->GetTimestamp() };
						uint32_t diffMs{ (packetTs - checkedPacketTs) * 1000 / clockRate };

						// Cleanup is finished if we found an item with recent enough packet, but also account
						// for out-of-order packets.
						if (diffMs < this->retransmissionBufferSize || packetTs < checkedPacketTs)
							break;

						// Reset (free RTP packet) the old storage item.
						resetStorageItem(checkedStorageItem);
						// Return into the pool.
						StorageItemPool.Return(checkedStorageItem);
						// Unfill the buffer start item.
						MS_ASSERT(
						  this->storageItemBuffer.Remove(this->bufferStartSeq), "Storage item must be used");
					}

					// Increase buffer start index.
					this->bufferStartSeq++;
				}
			}
		}

		// Store original packet and some extra info into the retrieved storage item.
		if (*clonedPacket)
		{
			storageItem->originalPacket = *clonedPacket;
			storageItem->ssrc           = (*clonedPacket)->GetSsrc();
			storageItem->sequenceNumber = (*clonedPacket)->GetSequenceNumber();
		}
		else
		{
			*clonedPacket               = packet->Clone();
			storageItem->originalPacket = *clonedPacket;
			storageItem->ssrc           = (*clonedPacket)->GetSsrc();
			storageItem->sequenceNumber = (*clonedPacket)->GetSequenceNumber();
		}
	}

	void RtpStreamSend::ClearBuffer()
	{
		MS_TRACE();

		this->storageItemBuffer.Clear();

		// Reset buffer.
		this->firstPacket    = true;
		this->bufferStartSeq = 0;
	}

	// This method looks for the requested RTP packets and inserts them into the
	// RetransmissionContainer vector (and sets to null the next position).
	//
	// If RTX is used the stored packet will be RTX encoded now (if not already
	// encoded in a previous resend).
	void RtpStreamSend::FillRetransmissionContainer(uint16_t seq, uint16_t bitmask)
	{
		MS_TRACE();

		// Ensure the container's first element is 0.
		RetransmissionContainer[0] = nullptr;

		// If NACK is not supported, exit.
		if (!this->params.useNack)
		{
			MS_WARN_TAG(rtx, "NACK not supported");

			return;
		}

		// Look for each requested packet.
		uint64_t nowMs      = DepLibUV::GetTimeMs();
		uint16_t rtt        = (this->rtt != 0u ? this->rtt : DefaultRtt);
		uint16_t currentSeq = seq;
		bool requested{ true };
		size_t containerIdx{ 0 };

		// Variables for debugging.
		uint16_t origBitmask = bitmask;
		uint16_t sentBitmask{ 0b0000000000000000 };
		bool isFirstPacket{ true };
		bool firstPacketSent{ false };
		uint8_t bitmaskCounter{ 0 };
		bool tooOldPacketFound{ false };

		while (requested || bitmask != 0)
		{
			bool sent = false;

			if (requested)
			{
				auto* storageItem = this->storageItemBuffer.Get(currentSeq);
				RTC::RtpPacket::SharedPtr packet{ nullptr };
				uint32_t diffMs;

				// Calculate the elapsed time between the max timestamp seen and the
				// requested packet's timestamp (in ms).
				if (storageItem)
				{
					packet = storageItem->originalPacket->Clone();
					// Put correct SSRC and sequence number into cloned packet.
					packet->SetSsrc(storageItem->ssrc);
					packet->SetSequenceNumber(storageItem->sequenceNumber);

					// Update MID RTP extension value.
					if (!this->mid.empty())
						packet->UpdateMid(mid);

					storageItem->clonedPacket = packet;

					uint32_t diffTs = this->maxPacketTs - packet->GetTimestamp();

					diffMs = diffTs * 1000 / this->params.clockRate;
				}

				// Packet not found.
				if (!storageItem)
				{
					// Do nothing.
				}
				// Don't resend the packet if older than MaxRetransmissionDelay ms.
				else if (diffMs > MaxRetransmissionDelay)
				{
					if (!tooOldPacketFound)
					{
						MS_WARN_TAG(
						  rtx,
						  "ignoring retransmission for too old packet "
						  "[seq:%" PRIu16 ", max age:%" PRIu32 "ms, packet age:%" PRIu32 "ms]",
						  packet->GetSequenceNumber(),
						  MaxRetransmissionDelay,
						  diffMs);

						tooOldPacketFound = true;
					}
				}
				// Don't resent the packet if it was resent in the last RTT ms.
				// clang-format off
				else if (
					storageItem->resentAtMs != 0u &&
					nowMs - storageItem->resentAtMs <= static_cast<uint64_t>(rtt)
				)
				// clang-format on
				{
					MS_DEBUG_TAG(
					  rtx,
					  "ignoring retransmission for a packet already resent in the last RTT ms "
					  "[seq:%" PRIu16 ", rtt:%" PRIu32 "]",
					  packet->GetSequenceNumber(),
					  rtt);
				}
				// Stored packet is valid for retransmission. Resend it.
				else
				{
					// If we use RTX and the packet has not yet been resent, encode it now.
					if (HasRtx())
					{
						// Increment RTX seq.
						++this->rtxSeq;

						if (!storageItem->rtxEncoded)
						{
							packet->RtxEncode(this->params.rtxPayloadType, this->params.rtxSsrc, this->rtxSeq);

							storageItem->rtxEncoded = true;
						}
						else
						{
							packet->SetSequenceNumber(this->rtxSeq);
						}
					}

					// Save when this packet was resent.
					storageItem->resentAtMs = nowMs;

					// Increase the number of times this packet was sent.
					storageItem->sentTimes++;

					// Store the storage item in the container and then increment its index.
					RetransmissionContainer[containerIdx++] = storageItem;

					sent = true;

					if (isFirstPacket)
						firstPacketSent = true;
				}
			}

			requested = (bitmask & 1) != 0;
			bitmask >>= 1;
			++currentSeq;

			if (!isFirstPacket)
			{
				sentBitmask |= (sent ? 1 : 0) << bitmaskCounter;
				++bitmaskCounter;
			}
			else
			{
				isFirstPacket = false;
			}
		}

		// If not all the requested packets was sent, log it.
		if (!firstPacketSent || origBitmask != sentBitmask)
		{
			MS_WARN_DEV(
			  "could not resend all packets [seq:%" PRIu16
			  ", first:%s, "
			  "bitmask:" MS_UINT16_TO_BINARY_PATTERN ", sent bitmask:" MS_UINT16_TO_BINARY_PATTERN "]",
			  seq,
			  firstPacketSent ? "yes" : "no",
			  MS_UINT16_TO_BINARY(origBitmask),
			  MS_UINT16_TO_BINARY(sentBitmask));
		}
		else
		{
			MS_DEBUG_DEV(
			  "all packets resent [seq:%" PRIu16 ", bitmask:" MS_UINT16_TO_BINARY_PATTERN "]",
			  seq,
			  MS_UINT16_TO_BINARY(origBitmask));
		}

		// Set the next container element to null.
		RetransmissionContainer[containerIdx] = nullptr;
	}

	void RtpStreamSend::UpdateScore(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();

		// Calculate number of packets sent in this interval.
		auto totalSent = this->transmissionCounter.GetPacketCount();
		auto sent      = totalSent - this->sentPriorScore;

		this->sentPriorScore = totalSent;

		// Calculate number of packets lost in this interval.
		uint32_t totalLost = report->GetTotalLost() > 0 ? report->GetTotalLost() : 0;
		uint32_t lost;

		if (totalLost < this->lostPriorScore)
			lost = 0;
		else
			lost = totalLost - this->lostPriorScore;

		this->lostPriorScore = totalLost;

		// Calculate number of packets repaired in this interval.
		auto totalRepaired = this->packetsRepaired;
		uint32_t repaired  = totalRepaired - this->repairedPriorScore;

		this->repairedPriorScore = totalRepaired;

		// Calculate number of packets retransmitted in this interval.
		auto totatRetransmitted = this->packetsRetransmitted;
		uint32_t retransmitted  = totatRetransmitted - this->retransmittedPriorScore;

		this->retransmittedPriorScore = totatRetransmitted;

		// We didn't send any packet.
		if (sent == 0)
		{
			RTC::RtpStream::UpdateScore(10);

			return;
		}

		if (lost > sent)
			lost = sent;

		if (repaired > lost)
			repaired = lost;

#if MS_LOG_DEV_LEVEL == 3
		MS_DEBUG_TAG(
		  score,
		  "[totalSent:%zu, totalLost:%" PRIi32 ", totalRepaired:%zu",
		  totalSent,
		  totalLost,
		  totalRepaired);

		MS_DEBUG_TAG(
		  score,
		  "fixed values [sent:%zu, lost:%" PRIu32 ", repaired:%" PRIu32 ", retransmitted:%" PRIu32,
		  sent,
		  lost,
		  repaired,
		  retransmitted);
#endif

		auto repairedRatio  = static_cast<float>(repaired) / static_cast<float>(sent);
		auto repairedWeight = std::pow(1 / (repairedRatio + 1), 4);

		MS_ASSERT(retransmitted >= repaired, "repaired packets cannot be more than retransmitted ones");

		if (retransmitted > 0)
			repairedWeight *= static_cast<float>(repaired) / retransmitted;

		lost -= repaired * repairedWeight;

		auto deliveredRatio = static_cast<float>(sent - lost) / static_cast<float>(sent);
		auto score          = static_cast<uint8_t>(std::round(std::pow(deliveredRatio, 4) * 10));

#if MS_LOG_DEV_LEVEL == 3
		MS_DEBUG_TAG(
		  score,
		  "[deliveredRatio:%f, repairedRatio:%f, repairedWeight:%f, new lost:%" PRIu32 ", score:%" PRIu8
		  "]",
		  deliveredRatio,
		  repairedRatio,
		  repairedWeight,
		  lost,
		  score);
#endif

		RtpStream::UpdateScore(score);
	}
} // namespace RTC
