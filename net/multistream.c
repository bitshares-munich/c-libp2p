#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include "libp2p/net/p2pnet.h"
#include "libp2p/record/message.h"
#include "libp2p/secio/secio.h"
#include "varint.h"
#include "libp2p/net/multistream.h"
#include "libp2p/utils/logger.h"
#include "multiaddr/multiaddr.h"

// NOTE: this is normally set to 5 seconds, but you may want to increase this during debugging
int multistream_default_timeout = 5;

/***
 * An implementation of the libp2p multistream
 */

/***
 * Close the Multistream interface
 * NOTE: This also closes the socket
 * @param stream_context a SessionContext
 * @returns true(1)
 */
int libp2p_net_multistream_close(void* stream_context) {
	struct SessionContext* secure_context = (struct SessionContext*)stream_context;
	struct Stream* stream = secure_context->default_stream;
	if (stream == NULL || stream->socket_descriptor == NULL)
		return 1;
	int socket_descriptor = *((int*)stream->socket_descriptor);
	close(socket_descriptor);
	free(stream->socket_descriptor);
	stream->socket_descriptor = NULL;
	if (stream->address != NULL)
		multiaddress_free(stream->address);
	stream->address = NULL;
	free(stream);
	secure_context->default_stream = NULL;
	secure_context->insecure_stream = NULL;
	secure_context->secure_stream = NULL;
	return 1;
}

/***
 * Check the stream to see if there is something to read
 * @param stream_context a SessionContext
 * @returns number of bytes to be read, or -1 if there was an error
 */
int libp2p_net_multistream_peek(void* stream_context) {
	if (stream_context == NULL)
		return -1;

	struct SessionContext* session_context = (struct SessionContext*)stream_context;
	struct Stream* stream = session_context->default_stream;
	if (stream == NULL)
		return -1;

	int socket_fd =  *((int*)stream->socket_descriptor);
	if (socket_fd < 0)
		return -1;

	int bytes = 0;
	if (ioctl(socket_fd, FIONREAD, &bytes) < 0) {
		// Ooff, we're having problems. Don't use this socket again.
		return -1;
	}
	return bytes;
}

/**
 * Write to an open multistream host
 * @param stream_context the session context
 * @param data the data to send
 * @param data_length the length of the data
 * @returns the number of bytes written
 */
int libp2p_net_multistream_write(void* stream_context, const unsigned char* data, size_t data_length) {
	struct SessionContext* session_context = (struct SessionContext*)stream_context;
	struct Stream* stream = session_context->default_stream;
	int num_bytes = 0;

	if (data_length > 0) { // only do this is if there is something to send
		// first send the size
		unsigned char varint[12];
		size_t varint_size = 0;
		varint_encode(data_length, &varint[0], 12, &varint_size);
		// now put the size with the data
		unsigned char* buffer = (unsigned char*)malloc(data_length + varint_size);
		if (buffer == NULL)
			return 0;
		memset(buffer, 0, data_length + varint_size);
		memcpy(buffer, varint, varint_size);
		memcpy(&buffer[varint_size], data, data_length);
		// determine if this should run through the secio protocol or not
		if (session_context->secure_stream == NULL) {
			int sd = *((int*)stream->socket_descriptor);
			// do a "raw" write
			num_bytes = socket_write(sd, (char*)varint, varint_size, 0);
			if (num_bytes == 0) {
				free(buffer);
				return 0;
			}
			// then send the actual data
			num_bytes += socket_write(sd, (char*)data, data_length, 0);
		} else {
			// write using secio
			num_bytes = stream->write(stream_context, buffer, data_length + varint_size);
		}
		free(buffer);
	}

	return num_bytes;
}

/**
 * Read from a multistream socket
 * @param socket_fd the socket file descriptor
 * @param results where to put the results. NOTE: this memory is allocated
 * @param results_size the size of the results in bytes
 * @param timeout_secs the seconds before a timeout
 * @returns number of bytes received
 */
int libp2p_net_multistream_read(void* stream_context, unsigned char** results, size_t* results_size, int timeout_secs) {
	struct SessionContext* session_context = (struct SessionContext*)stream_context;
	struct Stream* stream = session_context->default_stream;
	int bytes = 0;

	// TODO: this is arbitrary, and should be dynamic
	size_t buffer_size = 362144;
	char buffer[buffer_size];
	char* pos = buffer;
	size_t num_bytes_requested = 0, left = 0, already_read = 0;

	if (session_context->secure_stream == NULL) {
		int socketDescriptor = *( (int*) stream->socket_descriptor);
		// first read the varint
		while(1) {
			unsigned char c = '\0';
			bytes = socket_read(socketDescriptor, (char*)&c, 1, 0, timeout_secs);
			if (bytes <= 0) {
				// possible error
				if (bytes < 0)
					libp2p_logger_error("multistream", "socket_read returned %d reading socket %d\n", bytes, socketDescriptor);
				return 0;
			}
			pos[0] = c;
			if (c >> 7 == 0) {
				pos[1] = 0;
				num_bytes_requested = varint_decode((unsigned char*)buffer, strlen(buffer), NULL);
				break;
			}
			pos++;
		}
		if (num_bytes_requested <= 0) {
			libp2p_logger_debug("multistream", "Reading the varint returned %d on socket %d\n", num_bytes_requested, socketDescriptor);
			return 0;
		}

		left = num_bytes_requested;
		do {
			bytes = socket_read(socketDescriptor, &buffer[already_read], left, 0, timeout_secs);
			if (bytes < 0) {
				bytes = 0;
				if ( errno == EAGAIN ) {
					// do something intelligent
				} else {
					libp2p_logger_error("multistream", "socket read returned error %d on socket descriptor %d.\n", errno, socketDescriptor);
					return 0;
				}
			}
			left = left - bytes;
			already_read += bytes;
		} while (left > 0);

		if (already_read != num_bytes_requested)
			return 0;

		// parse the results, removing the leading size indicator
		*results = malloc(num_bytes_requested);
		if (*results == NULL)
			return 0;
		memcpy(*results, buffer, num_bytes_requested);
		*results_size = num_bytes_requested;
	} else { // use secio instead of raw read/writes
		unsigned char* read_from_stream;
		size_t size_read_from_stream;
		if (session_context->default_stream->read(session_context, &read_from_stream, &size_read_from_stream, timeout_secs) == 0) {
			return 0;
		}
		// pull out num_bytes_requested
		num_bytes_requested = varint_decode(read_from_stream, size_read_from_stream, &left);
		memcpy(buffer, read_from_stream, size_read_from_stream);
		free(read_from_stream);
		buffer_size = size_read_from_stream;
		while (num_bytes_requested > buffer_size - left) {
			// need to read more into buffer
			if (session_context->default_stream->read(session_context, &read_from_stream, &size_read_from_stream, timeout_secs) == 0) {
				return 0;
			}
			memcpy(&buffer[buffer_size], read_from_stream, size_read_from_stream);
			free(read_from_stream);
			buffer_size += size_read_from_stream;
		}
		*results = malloc(num_bytes_requested);
		*results_size = num_bytes_requested;
		if (*results == NULL) {
			libp2p_logger_error("multistream", "Unable to allocate %lu bytes of memory.", num_bytes_requested);
			return 0;
		}
		memcpy(*results, &buffer[left], num_bytes_requested);
	}

	return num_bytes_requested;
}


/**
 * Connect to a multistream host, and this includes the multistream handshaking.
 * @param hostname the host
 * @param port the port
 * @returns the socket file descriptor of the connection, or -1 on error
 */
struct Stream* libp2p_net_multistream_connect(const char* hostname, int port) {
	int retVal = -1, return_result = -1, socket = -1;
	unsigned char* results = NULL;
	size_t results_size;
	size_t num_bytes = 0;
	struct Stream* stream = NULL;

	uint32_t ip = hostname_to_ip(hostname);
	socket = socket_open4();

	// connect
	if (socket_connect4(socket, ip, port) != 0)
		goto exit;

	// send the multistream handshake
	char* protocol_buffer = "/multistream/1.0.0\n";

	stream = libp2p_net_multistream_stream_new(socket, hostname, port);
	if (stream == NULL)
		goto exit;

	struct SessionContext session;
	session.insecure_stream = stream;
	session.secure_stream = NULL;
	session.default_stream = stream;

	// try to receive the protocol id
	return_result = libp2p_net_multistream_read(&session, &results, &results_size, multistream_default_timeout);
	if (return_result == 0 || results_size < 1)
		goto exit;

	num_bytes = libp2p_net_multistream_write(&session, (unsigned char*)protocol_buffer, strlen(protocol_buffer));
	if (num_bytes <= 0)
		goto exit;

	if (strstr((char*)results, "multistream") == NULL)
		goto exit;

	// we are now in the loop, so we can switch to another protocol (i.e. /secio/1.0.0)

	retVal = socket;
	exit:
	if (results != NULL)
		free(results);
	if (retVal < 0 && stream != NULL) {
		libp2p_net_multistream_stream_free(stream);
		stream = NULL;
	}
	if (retVal < 0 && socket > 0)
		close(socket);
	return stream;
}

/**
 * Negotiate the multistream protocol by sending and receiving the protocol id. This is a server side function.
 * Servers should send the protocol ID, and then expect it back.
 * NOTE: the SessionContext should already contain the connected stream. If not, use
 * libp2p_net_multistream_connect instead of this method.
 *
 * @param session the struct Session, which contains all the context info
 * @returns true(1) on success, or false(0)
 */
int libp2p_net_multistream_negotiate(struct SessionContext* session) {
	const char* protocolID = "/multistream/1.0.0\n";
	unsigned char* results = NULL;
	size_t results_length = 0;
	int retVal = 0;
	// send the protocol id
	if (!libp2p_net_multistream_write(session, (unsigned char*)protocolID, strlen(protocolID)))
		goto exit;
	// expect the same back
	libp2p_net_multistream_read(session, &results, &results_length, multistream_default_timeout);
	if (results_length == 0)
		goto exit;
	if (strncmp((char*)results, protocolID, strlen(protocolID)) != 0)
		goto exit;
	retVal = 1;
	exit:
	if (results != NULL)
		free(results);
	return retVal;
}


/**
 * Expect to read a message
 * @param fd the socket file descriptor
 * @returns the retrieved message, or NULL
 */
/*
struct Libp2pMessage* libp2p_net_multistream_get_message(struct Stream* stream) {
	int retVal = 0;
	unsigned char* results = NULL;
	size_t results_size = 0;
	struct Libp2pMessage* msg = NULL;
	// read what they sent
	libp2p_net_multistream_read(stream, &results, &results_size);
	// unprotobuf it
	if (!libp2p_message_protobuf_decode(results, results_size, &msg))
		goto exit;
	// clean up
	retVal = 1;
	exit:
	if (results != NULL)
		free(results);
	if (retVal != 1 && msg != NULL)
		libp2p_message_free(msg);

	return msg;
}
*/

void libp2p_net_multistream_stream_free(struct Stream* stream) {
	if (stream != NULL) {
		if (stream->socket_descriptor != NULL) {
			close( *((int*)stream->socket_descriptor));
			free(stream->socket_descriptor);
		}
		if (stream->address != NULL)
			multiaddress_free(stream->address);
		free(stream);
	}
}

/**
 * Create a new MultiStream structure
 * @param socket_fd the file descriptor
 * @param ip the IP address
 * @param port the port
 */
struct Stream* libp2p_net_multistream_stream_new(int socket_fd, const char* ip, int port) {
	struct Stream* out = (struct Stream*)malloc(sizeof(struct Stream));
	if (out != NULL) {
		out->socket_descriptor = malloc(sizeof(int));
		*((int*)out->socket_descriptor) = socket_fd;
		int res = *((int*)out->socket_descriptor);
		if (res != socket_fd) {
			libp2p_net_multistream_stream_free(out);
			return NULL;
		}
		out->close = libp2p_net_multistream_close;
		out->read = libp2p_net_multistream_read;
		out->write = libp2p_net_multistream_write;
		out->peek = libp2p_net_multistream_peek;
		char str[strlen(ip) + 50];
		sprintf(str, "/ip4/%s/tcp/%d", ip, port);
		out->address = multiaddress_new_from_string(str);
	}
	return out;
}

