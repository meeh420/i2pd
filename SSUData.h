#ifndef SSU_DATA_H__
#define SSU_DATA_H__

#include <inttypes.h>
#include <map>
#include <vector>
#include "I2NPProtocol.h"

namespace i2p
{
namespace ssu
{

	// data flags
	const uint8_t DATA_FLAG_EXTENDED_DATA_INCLUDED = 0x02;
	const uint8_t DATA_FLAG_WANT_REPLY = 0x04;
	const uint8_t DATA_FLAG_REQUEST_PREVIOUS_ACKS = 0x08;
	const uint8_t DATA_FLAG_EXPLICIT_CONGESTION_NOTIFICATION = 0x10;
	const uint8_t DATA_FLAG_ACK_BITFIELDS_INCLUDED = 0x40;
	const uint8_t DATA_FLAG_EXPLICIT_ACKS_INCLUDED = 0x80;	

	class SSUSession;
	class SSUData
	{
		public:

			SSUData (SSUSession& session);
			~SSUData ();

			void ProcessMessage (uint8_t * buf, size_t len);
			void Send (i2p::I2NPMessage * msg);

		private:

			void SendMsgAck (uint32_t msgID);
			void ProcessSentMessageAck (uint32_t msgID);

		private:

			struct IncompleteMessage
			{
				I2NPMessage * msg;
				uint8_t nextFragmentNum;	

				IncompleteMessage (I2NPMessage * m): msg (m), nextFragmentNum (1) {};
			};

			SSUSession& m_Session;
			std::map<uint32_t, IncompleteMessage *> m_IncomleteMessages;
			std::map<uint32_t, std::vector<uint8_t *> > m_SentMessages; // msgID -> fragments	
	};	
}
}

#endif

