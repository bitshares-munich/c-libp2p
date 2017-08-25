#pragma once
#include "libp2p/conn/session.h"
#include "libp2p/utils/vector.h"

/***
 * An "interface" for different IPFS protocols
 */
struct Libp2pProtocolHandler {
	/**
	 * A protocol dependent context (often an IpfsNode pointer, but libp2p doesn't know about that)
	 */
	void* context;
	/**
	 * Determines if this protocol can handle the incoming message
	 * @param incoming the incoming data
	 * @param incoming_size the size of the incoming data buffer
	 * @returns true(1) if it can handle this message, false(0) if not
	 */
	int (*CanHandle)(const uint8_t* incoming, size_t incoming_size);
	/***
	 * Handles the message
	 * @param incoming the incoming data buffer
	 * @param incoming_size the size of the incoming data buffer
	 * @param session_context the information about the incoming connection
	 * @param protocol_context the protocol-dependent context
	 * @returns 0 if the caller should not continue looping, <0 on error, >0 on success
	 */
	int (*HandleMessage)(const uint8_t* incoming, size_t incoming_size, struct SessionContext* session_context, void* protocol_context);

	/**
	 * Shutting down. Clean up any memory allocations
	 * @param protocol_context the context
	 * @returns true(1)
	 */
	int (*Shutdown)(void* protocol_context);
};

int libp2p_protocol_marshal(const uint8_t* incoming, size_t incoming_size, struct SessionContext* context, struct Libp2pVector* protocol_handlers);
