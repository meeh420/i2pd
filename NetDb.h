#ifndef NETDB_H__
#define NETDB_H__

#include <inttypes.h>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <boost/filesystem.hpp>
#include "Queue.h"
#include "I2NPProtocol.h"
#include "RouterInfo.h"
#include "LeaseSet.h"
#include "Tunnel.h"
#include "AddressBook.h"

namespace i2p
{
namespace data
{		
	class RequestedDestination
	{
		public:

			RequestedDestination (const IdentHash& destination, bool isLeaseSet, bool isExploratory = false):
				m_Destination (destination), m_IsLeaseSet (isLeaseSet), m_IsExploratory (isExploratory), 
				m_LastRouter (nullptr), m_CreationTime (0) {};
			
			const IdentHash& GetDestination () const { return m_Destination; };
			int GetNumExcludedPeers () const { return m_ExcludedPeers.size (); };
			const std::set<IdentHash>& GetExcludedPeers () { return m_ExcludedPeers; };
			void ClearExcludedPeers ();
			const RouterInfo * GetLastRouter () const { return m_LastRouter; };
			bool IsExploratory () const { return m_IsExploratory; };
			bool IsLeaseSet () const { return m_IsLeaseSet; };
			bool IsExcluded (const IdentHash& ident) const { return m_ExcludedPeers.count (ident); };
			uint64_t GetCreationTime () const { return m_CreationTime; };
			I2NPMessage * CreateRequestMessage (const RouterInfo * router, const i2p::tunnel::InboundTunnel * replyTunnel);
			I2NPMessage * CreateRequestMessage (const IdentHash& floodfill);
						
		private:

			IdentHash m_Destination;
			bool m_IsLeaseSet, m_IsExploratory;
			std::set<IdentHash> m_ExcludedPeers;
			const RouterInfo * m_LastRouter;
			uint64_t m_CreationTime;
	};	
	
	class NetDb
	{
		public:

			NetDb ();
			~NetDb ();

			void Start ();
			void Stop ();
			
			void AddRouterInfo (uint8_t * buf, int len);
			void AddLeaseSet (uint8_t * buf, int len);
			RouterInfo * FindRouter (const IdentHash& ident) const;
			LeaseSet * FindLeaseSet (const IdentHash& destination) const;
			const IdentHash * FindAddress (const std::string& address) { return m_AddressBook.FindAddress (address); }; // TODO: move AddressBook away from NetDb

			void Subscribe (const IdentHash& ident); // keep LeaseSets upto date			
			void Unsubscribe (const IdentHash& ident);	
			void RequestDestination (const IdentHash& destination, bool isLeaseSet = false);
						
			void HandleDatabaseStoreMsg (uint8_t * buf, size_t len);
			void HandleDatabaseSearchReplyMsg (I2NPMessage * msg);
			
			const RouterInfo * GetRandomRouter (const RouterInfo * compatibleWith = nullptr) const;

			void PostI2NPMsg (I2NPMessage * msg);

			// for web interface
			int GetNumRouters () const { return m_RouterInfos.size (); };
			int GetNumFloodfills () const { return m_Floodfills.size (); };
			int GetNumLeaseSets () const { return m_LeaseSets.size (); };
			
		private:

			bool CreateNetDb(boost::filesystem::path directory);
			void Load (const char * directory);
			void SaveUpdated (const char * directory);
			void Run (); // exploratory thread
			void Explore (int numDestinations);
			void Publish ();
			void ValidateSubscriptions ();
			const RouterInfo * GetClosestFloodfill (const IdentHash& destination, const std::set<IdentHash>& excluded) const;
			void KeyspaceRotation ();

			RequestedDestination * CreateRequestedDestination (const IdentHash& dest, 
				bool isLeaseSet, bool isExploratory = false);
			void DeleteRequestedDestination (const IdentHash& dest);
			void DeleteRequestedDestination (RequestedDestination * dest);
		
		private:

			std::map<IdentHash, LeaseSet *> m_LeaseSets;
			std::map<IdentHash, RouterInfo *> m_RouterInfos;
			std::vector<RouterInfo *> m_Floodfills;
			std::map<IdentHash, RequestedDestination *> m_RequestedDestinations;
			std::set<IdentHash> m_Subscriptions;
			
			bool m_IsRunning;
			int m_ReseedRetries;
			std::thread * m_Thread;	
			i2p::util::Queue<I2NPMessage> m_Queue; // of I2NPDatabaseStoreMsg
			AddressBook m_AddressBook;

			static const char m_NetDbPath[];
	};

	extern NetDb netdb;
}
}

#endif
