#include <string.h>
#include <atomic>
#include "I2PEndian.h"
#include <cryptopp/sha.h>
#include <cryptopp/gzip.h>
#include "ElGamal.h"
#include "Timestamp.h"
#include "RouterContext.h"
#include "NetDb.h"
#include "Tunnel.h"
#include "base64.h"
#include "Transports.h"
#include "Garlic.h"
#include "I2NPProtocol.h"

namespace i2p
{

	I2NPMessage * NewI2NPMessage ()
	{
		I2NPMessage * msg = new I2NPMessage;
		msg->offset = 2; // reserve 2 bytes for NTCP header, should reserve more for SSU in future
		msg->len = sizeof (I2NPHeader) + 2;
		msg->from = nullptr;
		return msg;
	}
	
	void DeleteI2NPMessage (I2NPMessage * msg)
	{
		delete msg;
	}	

	static std::atomic<uint32_t> I2NPmsgID(0); // TODO: create class
	void FillI2NPMessageHeader (I2NPMessage * msg, I2NPMessageType msgType, uint32_t replyMsgID)
	{
		I2NPHeader * header = msg->GetHeader ();
		header->typeID = msgType;
		if (replyMsgID) // for tunnel creation
			header->msgID = htobe32 (replyMsgID); 
		else
		{	
			header->msgID = htobe32 (I2NPmsgID);
			I2NPmsgID++;
		}	
		header->expiration = htobe64 (i2p::util::GetMillisecondsSinceEpoch () + 5000); // TODO: 5 secs is a magic number
		int len = msg->GetLength () - sizeof (I2NPHeader);
		header->size = htobe16 (len);
		uint8_t hash[32];
		CryptoPP::SHA256().CalculateDigest(hash, msg->GetPayload (), len);
		header->chks = hash[0];
	}	

	void RenewI2NPMessageHeader (I2NPMessage * msg)
	{
		if (msg)
		{
			I2NPHeader * header = msg->GetHeader ();
			header->msgID = htobe32 (I2NPmsgID);
			I2NPmsgID++;
			header->expiration = htobe64 (i2p::util::GetMillisecondsSinceEpoch () + 5000); 		
		}
	}

	I2NPMessage * CreateI2NPMessage (I2NPMessageType msgType, const uint8_t * buf, int len, uint32_t replyMsgID)
	{
		I2NPMessage * msg = NewI2NPMessage ();
		memcpy (msg->GetPayload (), buf, len);
		msg->len += len;
		FillI2NPMessageHeader (msg, msgType, replyMsgID);
		return msg;
	}	

	I2NPMessage * CreateI2NPMessage (const uint8_t * buf, int len)
	{
		I2NPMessage * msg = NewI2NPMessage ();
		memcpy (msg->GetBuffer (), buf, len);
		msg->len = msg->offset + len;
		return msg;
	}	
	
	I2NPMessage * CreateDeliveryStatusMsg (uint32_t msgID)
	{
		I2NPDeliveryStatusMsg msg;
		if (msgID)
		{
			msg.msgID = htobe32 (msgID);
			msg.timestamp = htobe64 (i2p::util::GetMillisecondsSinceEpoch ());
		}
		else // for SSU establishment
		{
			msg.msgID = htobe32 (i2p::context.GetRandomNumberGenerator ().GenerateWord32 ());
			msg.timestamp = htobe64 (2); // netID = 2
 		}
		return CreateI2NPMessage (eI2NPDeliveryStatus, (uint8_t *)&msg, sizeof (msg));
	}

	I2NPMessage * CreateDatabaseLookupMsg (const uint8_t * key, const uint8_t * from, 
		uint32_t replyTunnelID, bool exploratory, std::set<i2p::data::IdentHash> * excludedPeers,
	    bool encryption)
	{
		I2NPMessage * m = NewI2NPMessage ();
		uint8_t * buf = m->GetPayload ();
		memcpy (buf, key, 32); // key
		buf += 32;
		memcpy (buf, from, 32); // from
		buf += 32;
		if (replyTunnelID)
		{
			*buf = encryption ? 0x03: 0x01; // set delivery flag
			*(uint32_t *)(buf+1) = htobe32 (replyTunnelID);
			buf += 5;
		}
		else
		{	
			encryption = false; // encryption can we set for tunnels only
			*buf = 0; // flag
			buf++;
		}	
		
		if (exploratory)
		{
			*(uint16_t *)buf = htobe16 (1); // one exlude record
			buf += 2;
			// reply with non-floodfill routers only
			memset (buf, 0, 32);
			buf += 32;
		}
		else
		{
			if (excludedPeers)
			{
				int cnt = excludedPeers->size ();
				*(uint16_t *)buf = htobe16 (cnt);
				buf += 2;
				for (auto& it: *excludedPeers)
				{
					memcpy (buf, it, 32);
					buf += 32;
				}	
			}
			else
			{	
				// nothing to exclude
				*(uint16_t *)buf = htobe16 (0);
				buf += 2;
			}	
		}	
		if (encryption)
		{
			// session key and tag for reply
			auto& rnd = i2p::context.GetRandomNumberGenerator ();
			rnd.GenerateBlock (buf, 32); // key
			buf[32] = 1; // 1 tag
			rnd.GenerateBlock (buf + 33, 32); // tag
			i2p::garlic::routing.AddSessionKey (buf, buf + 33); // introduce new key-tag to garlic engine
			buf += 65;
		}	
		m->len += (buf - m->GetPayload ()); 
		FillI2NPMessageHeader (m, eI2NPDatabaseLookup);
		return m; 
	}	

	void HandleDatabaseLookupMsg (uint8_t * buf, size_t len)
	{
		char key[48];
		int l = i2p::data::ByteStreamToBase64 (buf, 32, key, 48);
		key[l] = 0;
		LogPrint ("DatabaseLookup for ", key, " recieved");
		uint8_t flag = buf[64];
		uint32_t replyTunnelID = 0;
		if (flag & 0x01) //reply to yunnel
			replyTunnelID = be32toh (*(uint32_t *)(buf + 64));
		// TODO: implement search. We send non-found for now
		I2NPMessage * replyMsg = CreateDatabaseSearchReply (buf);
		if (replyTunnelID)
			i2p::tunnel::tunnels.GetNextOutboundTunnel ()->SendTunnelDataMsg (buf+32, replyTunnelID, replyMsg);
		else
			i2p::transports.SendMessage (buf, replyMsg);
	}	

	I2NPMessage * CreateDatabaseSearchReply (const i2p::data::IdentHash& ident)
	{
		I2NPMessage * m = NewI2NPMessage ();
		uint8_t * buf = m->GetPayload ();
		memcpy (buf, ident, 32);
		buf[32] = 0; // TODO:
		memcpy (buf + 33, i2p::context.GetRouterInfo ().GetIdentHash (), 32);
		m->len += 65;
		FillI2NPMessageHeader (m, eI2NPDatabaseSearchReply);
		return m; 
	}	
	
	I2NPMessage * CreateDatabaseStoreMsg ()
	{
		I2NPMessage * m = NewI2NPMessage ();
		I2NPDatabaseStoreMsg * msg = (I2NPDatabaseStoreMsg *)m->GetPayload ();		

		memcpy (msg->key, context.GetRouterInfo ().GetIdentHash (), 32);
		msg->type = 0;
		msg->replyToken = 0;
		
		CryptoPP::Gzip compressor;
		compressor.Put (context.GetRouterInfo ().GetBuffer (), context.GetRouterInfo ().GetBufferLen ());
		compressor.MessageEnd();
		// WARNING!!! MaxRetrievable() return uint64_t. ���� ����������, ��� ���-�� �� ���
		int size = compressor.MaxRetrievable ();
		uint8_t * buf = m->GetPayload () + sizeof (I2NPDatabaseStoreMsg);
		*(uint16_t *)buf = htobe16 (size); // size
		buf += 2;
		compressor.Get (buf, size); 
		m->len += sizeof (I2NPDatabaseStoreMsg) + 2 + size; // payload size
		FillI2NPMessageHeader (m, eI2NPDatabaseStore);
		
		return m;
	}	


	I2NPBuildRequestRecordClearText CreateBuildRequestRecord (
		const uint8_t * ourIdent, uint32_t receiveTunnelID, 
	    const uint8_t * nextIdent, uint32_t nextTunnelID, 
	    const uint8_t * layerKey,const uint8_t * ivKey,                                                                 
	    const uint8_t * replyKey, const uint8_t * replyIV, uint32_t nextMessageID,
	          bool isGateway, bool isEndpoint)
	{
		I2NPBuildRequestRecordClearText clearText;	
		clearText.receiveTunnel = htobe32 (receiveTunnelID); 		
		clearText.nextTunnel = htobe32(nextTunnelID);
		memcpy (clearText.layerKey, layerKey, 32);
		memcpy (clearText.ivKey, ivKey, 32);
		memcpy (clearText.replyKey, replyKey, 32);
		memcpy (clearText.replyIV, replyIV, 16);
		clearText.flag = 0;
		if (isGateway) clearText.flag |= 0x80;
		if (isEndpoint) clearText.flag |= 0x40;
		memcpy (clearText.ourIdent, ourIdent, 32);
		memcpy (clearText.nextIdent, nextIdent, 32);
		clearText.requestTime = i2p::util::GetHoursSinceEpoch (); 
		clearText.nextMessageID = htobe32(nextMessageID);
		return clearText;
	}	

	void EncryptBuildRequestRecord (const i2p::data::RouterInfo& router, 
		const I2NPBuildRequestRecordClearText& clearText,
	    I2NPBuildRequestRecordElGamalEncrypted& record)
	{
		router.GetElGamalEncryption ()->Encrypt ((uint8_t *)&clearText, sizeof(clearText), record.encrypted);
		memcpy (record.toPeer, (const uint8_t *)router.GetIdentHash (), 16);
	}	
	
	bool HandleBuildRequestRecords (int num, I2NPBuildRequestRecordElGamalEncrypted * records, I2NPBuildRequestRecordClearText& clearText)
	{
		for (int i = 0; i < num; i++)
		{	
			if (!memcmp (records[i].toPeer, (const uint8_t *)i2p::context.GetRouterInfo ().GetIdentHash (), 16))
			{	
				LogPrint ("Record ",i," is ours");	
			
				i2p::crypto::ElGamalDecrypt (i2p::context.GetPrivateKey (), records[i].encrypted, (uint8_t *)&clearText);

				i2p::tunnel::TransitTunnel * transitTunnel = 
					i2p::tunnel::CreateTransitTunnel (
					be32toh (clearText.receiveTunnel), 
					clearText.nextIdent, be32toh (clearText.nextTunnel),
				    clearText.layerKey, clearText.ivKey, 
				    clearText.flag & 0x80, clearText.flag & 0x40);
				i2p::tunnel::tunnels.AddTransitTunnel (transitTunnel);
				// replace record to reply
				I2NPBuildResponseRecord * reply = (I2NPBuildResponseRecord *)(records + i);
				reply->ret = 0;
				//TODO: fill filler
				CryptoPP::SHA256().CalculateDigest(reply->hash, reply->padding, sizeof (reply->padding) + 1); // + 1 byte of ret
				// encrypt reply
				i2p::crypto::CBCEncryption encryption;
				for (int j = 0; j < num; j++)
				{
					encryption.SetKey (clearText.replyKey);
					encryption.SetIV (clearText.replyIV);
					encryption.Encrypt((uint8_t *)(records + j), sizeof (records[j]), (uint8_t *)(records + j)); 
				}
				return true;
			}	
		}	
		return false;
	}

	void HandleVariableTunnelBuildMsg (uint32_t replyMsgID, uint8_t * buf, size_t len)
	{	
		int num = buf[0];
		LogPrint ("VariableTunnelBuild ", num, " records");

		i2p::tunnel::Tunnel * tunnel =  i2p::tunnel::tunnels.GetPendingTunnel (replyMsgID);
		if (tunnel)
		{
			// endpoint of inbound tunnel
			LogPrint ("VariableTunnelBuild reply for tunnel ", tunnel->GetTunnelID ());
			if (tunnel->HandleTunnelBuildResponse (buf, len))
			{
				LogPrint ("Inbound tunnel ", tunnel->GetTunnelID (), " has been created");
				i2p::tunnel::tunnels.AddInboundTunnel (static_cast<i2p::tunnel::InboundTunnel *>(tunnel));
			}
			else
			{
				LogPrint ("Inbound tunnel ", tunnel->GetTunnelID (), " has been declined");
				delete tunnel;
			}	
		}
		else
		{
			I2NPBuildRequestRecordElGamalEncrypted * records = (I2NPBuildRequestRecordElGamalEncrypted *)(buf+1); 
			I2NPBuildRequestRecordClearText clearText;	
			if (HandleBuildRequestRecords (num, records, clearText))
			{
				if (clearText.flag & 0x40) // we are endpoint of outboud tunnel
				{
					// so we send it to reply tunnel 
					i2p::transports.SendMessage (clearText.nextIdent, 
						CreateTunnelGatewayMsg (be32toh (clearText.nextTunnel),
							eI2NPVariableTunnelBuildReply, buf, len, 
						    be32toh (clearText.nextMessageID)));                         
				}	
				else	
					i2p::transports.SendMessage (clearText.nextIdent, 
						CreateI2NPMessage (eI2NPVariableTunnelBuild, buf, len, be32toh (clearText.nextMessageID)));
			}	
		}	
	}

	void HandleTunnelBuildMsg (uint8_t * buf, size_t len)
	{
		I2NPBuildRequestRecordClearText clearText;	
		if (HandleBuildRequestRecords (NUM_TUNNEL_BUILD_RECORDS, (I2NPBuildRequestRecordElGamalEncrypted *)buf, clearText))
		{
			if (clearText.flag & 0x40) // we are endpoint of outbound tunnel
			{
				// so we send it to reply tunnel 
				i2p::transports.SendMessage (clearText.nextIdent, 
					CreateTunnelGatewayMsg (be32toh (clearText.nextTunnel),
						eI2NPTunnelBuildReply, buf, len, 
					    be32toh (clearText.nextMessageID)));                         
			}	
			else	
				i2p::transports.SendMessage (clearText.nextIdent, 
					CreateI2NPMessage (eI2NPTunnelBuild, buf, len, be32toh (clearText.nextMessageID)));
		} 
	}

	void HandleVariableTunnelBuildReplyMsg (uint32_t replyMsgID, uint8_t * buf, size_t len)
	{	
		LogPrint ("VariableTunnelBuildReplyMsg replyMsgID=", replyMsgID);
		i2p::tunnel::Tunnel * tunnel = i2p::tunnel::tunnels.GetPendingTunnel (replyMsgID);
		if (tunnel)
		{	
			// reply for outbound tunnel
			if (tunnel->HandleTunnelBuildResponse (buf, len))
			{	
				LogPrint ("Outbound tunnel ", tunnel->GetTunnelID (), " has been created");
				i2p::tunnel::tunnels.AddOutboundTunnel (static_cast<i2p::tunnel::OutboundTunnel *>(tunnel));
			}	
			else
			{	
				LogPrint ("Outbound tunnel ", tunnel->GetTunnelID (), " has been declined");
				delete tunnel;
			}	
		}	
		else
			LogPrint ("Pending tunnel for message ", replyMsgID, " not found");
	}


	I2NPMessage * CreateTunnelDataMsg (const uint8_t * buf)
	{
		I2NPMessage * msg = NewI2NPMessage ();
		memcpy (msg->GetPayload (), buf, i2p::tunnel::TUNNEL_DATA_MSG_SIZE);
		msg->len += i2p::tunnel::TUNNEL_DATA_MSG_SIZE; 
		FillI2NPMessageHeader (msg, eI2NPTunnelData);
		return msg;
	}	

	I2NPMessage * CreateTunnelDataMsg (uint32_t tunnelID, const uint8_t * payload)	
	{
		I2NPMessage * msg = NewI2NPMessage ();
		memcpy (msg->GetPayload () + 4, payload, i2p::tunnel::TUNNEL_DATA_MSG_SIZE - 4);
		*(uint32_t *)(msg->GetPayload ()) = htobe32 (tunnelID);
		msg->len += i2p::tunnel::TUNNEL_DATA_MSG_SIZE; 
		FillI2NPMessageHeader (msg, eI2NPTunnelData);
		return msg;
	}	
	
	I2NPMessage * CreateTunnelGatewayMsg (uint32_t tunnelID, const uint8_t * buf, size_t len)
	{
		I2NPMessage * msg = NewI2NPMessage ();
		TunnelGatewayHeader * header = (TunnelGatewayHeader *)msg->GetPayload ();
		header->tunnelID = htobe32 (tunnelID);
		header->length = htobe16 (len);
		memcpy (msg->GetPayload () + sizeof (TunnelGatewayHeader), buf, len);
		msg->len += sizeof (TunnelGatewayHeader) + len;
		FillI2NPMessageHeader (msg, eI2NPTunnelGateway);
		return msg;
	}	

	I2NPMessage * CreateTunnelGatewayMsg (uint32_t tunnelID, I2NPMessage * msg)
	{
		if (msg->offset >= sizeof (I2NPHeader) + sizeof (TunnelGatewayHeader))
		{
			// message is capable to be used without copying
			TunnelGatewayHeader * header = (TunnelGatewayHeader *)(msg->GetBuffer () - sizeof (TunnelGatewayHeader));
			header->tunnelID = htobe32 (tunnelID);
			int len = msg->GetLength ();
			header->length = htobe16 (len);
			msg->offset -= (sizeof (I2NPHeader) + sizeof (TunnelGatewayHeader));
			msg->len = msg->offset + sizeof (I2NPHeader) + sizeof (TunnelGatewayHeader) +len;
			FillI2NPMessageHeader (msg, eI2NPTunnelGateway);
			return msg;
		}
		else
		{	
			I2NPMessage * msg1 = CreateTunnelGatewayMsg (tunnelID, msg->GetBuffer (), msg->GetLength ());
			DeleteI2NPMessage (msg);
			return msg1;
		}	                               
	}

	I2NPMessage * CreateTunnelGatewayMsg (uint32_t tunnelID, I2NPMessageType msgType, 
		const uint8_t * buf, size_t len, uint32_t replyMsgID)
	{
		I2NPMessage * msg = NewI2NPMessage ();
		size_t gatewayMsgOffset = sizeof (I2NPHeader) + sizeof (TunnelGatewayHeader);
		msg->offset += gatewayMsgOffset;
		msg->len += gatewayMsgOffset;
		memcpy (msg->GetPayload (), buf, len);
		msg->len += len;
		FillI2NPMessageHeader (msg, msgType, replyMsgID); // create content message
		len = msg->GetLength ();
		msg->offset -= gatewayMsgOffset;
		TunnelGatewayHeader * header = (TunnelGatewayHeader *)msg->GetPayload ();
		header->tunnelID = htobe32 (tunnelID);
		header->length = htobe16 (len);
		FillI2NPMessageHeader (msg, eI2NPTunnelGateway); // gateway message
		return msg;
	}	
	
	void HandleTunnelGatewayMsg (I2NPMessage * msg)
	{		
		TunnelGatewayHeader * header = (TunnelGatewayHeader *)msg->GetPayload ();
		uint32_t tunnelID = be32toh(header->tunnelID);
		uint16_t len = be16toh(header->length);
		// we make payload as new I2NP message to send
		msg->offset += sizeof (I2NPHeader) + sizeof (TunnelGatewayHeader);
		msg->len = msg->offset + len;
		LogPrint ("TunnelGateway of ", (int)len, " bytes for tunnel ", (unsigned int)tunnelID, ". Msg type ", (int)msg->GetHeader()->typeID);
		if (msg->GetHeader()->typeID == eI2NPDatabaseStore)
		{
			// transit DatabaseStore my contain new/updated RI
			auto ds = NewI2NPMessage ();
			*ds = *msg;
			i2p::data::netdb.PostI2NPMsg (ds);
		}	
		i2p::tunnel::TransitTunnel * tunnel =  i2p::tunnel::tunnels.GetTransitTunnel (tunnelID);
		if (tunnel)
			tunnel->SendTunnelDataMsg (msg);
		else
		{	
			LogPrint ("Tunnel ", (unsigned int)tunnelID, " not found");
			i2p::DeleteI2NPMessage (msg);
		}	
	}	

	size_t GetI2NPMessageLength (uint8_t * msg)
	{
		I2NPHeader * header = (I2NPHeader *)msg;
		return be16toh (header->size) + sizeof (I2NPHeader);
	}	
	
	void HandleI2NPMessage (uint8_t * msg, size_t len)
	{
		I2NPHeader * header = (I2NPHeader *)msg;
		uint32_t msgID = be32toh (header->msgID);	
		LogPrint ("I2NP msg received len=", len,", type=", (int)header->typeID, ", msgID=", (unsigned int)msgID);

		uint8_t * buf = msg + sizeof (I2NPHeader);
		int size = be16toh (header->size);
		switch (header->typeID)
		{	
			case eI2NPVariableTunnelBuild:
				LogPrint ("VariableTunnelBuild");
				HandleVariableTunnelBuildMsg  (msgID, buf, size);
			break;	
			case eI2NPVariableTunnelBuildReply:
				LogPrint ("VariableTunnelBuildReply");
				HandleVariableTunnelBuildReplyMsg (msgID, buf, size);
			break;	
			case eI2NPTunnelBuild:
				LogPrint ("TunnelBuild");
				HandleTunnelBuildMsg  (buf, size);
			break;	
			case eI2NPTunnelBuildReply:
				LogPrint ("TunnelBuildReply");
				// TODO:
			break;	
			case eI2NPDatabaseLookup:
				LogPrint ("DatabaseLookup");
				HandleDatabaseLookupMsg (buf, size);
			break;	
			default:
				LogPrint ("Unexpected message ", (int)header->typeID);
		}	
	}

	void HandleI2NPMessage (I2NPMessage * msg)
	{
		if (msg)
		{	
			switch (msg->GetHeader ()->typeID)
			{	
				case eI2NPTunnelData:
					LogPrint ("TunnelData");
					i2p::tunnel::tunnels.PostTunnelData (msg);
				break;	
				case eI2NPTunnelGateway:
					LogPrint ("TunnelGateway");
					HandleTunnelGatewayMsg (msg);
				break;
				case eI2NPGarlic:
					LogPrint ("Garlic");
					i2p::garlic::routing.HandleGarlicMessage (msg);
				break;
				case eI2NPDatabaseStore:
					LogPrint ("DatabaseStore");
					i2p::data::netdb.PostI2NPMsg (msg);
				break;	
				case eI2NPDatabaseSearchReply:
					LogPrint ("DatabaseSearchReply");
					i2p::data::netdb.PostI2NPMsg (msg);
				break;					
				case eI2NPDeliveryStatus:
					LogPrint ("DeliveryStatus");
					if (msg->from && msg->from->GetTunnelPool ())
						msg->from->GetTunnelPool ()->ProcessDeliveryStatus (msg);
					else
					{
						i2p::garlic::routing.HandleDeliveryStatusMessage (msg->GetPayload (), msg->GetLength ()); 
						DeleteI2NPMessage (msg);
					}			
				break;	
				default:
					HandleI2NPMessage (msg->GetBuffer (), msg->GetLength ());
					DeleteI2NPMessage (msg);
			}	
		}	
	}	
}
