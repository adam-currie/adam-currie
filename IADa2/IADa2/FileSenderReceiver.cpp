#include <string.h>
#include "FileSenderReceiver.h"
#include "FileWrapper.h"

using namespace std;
using namespace net;


FileSenderReceiver::FileSenderReceiver() : 
	connection(PROTOCOL_ID, TIME_OUT, UINT_MAX){
	sendInterval = .01f;
}

//todo: document exceptions, CannotOpenFileException, ConnectFailedException, LostConnectionException
_int64 FileSenderReceiver::SendFile(std::string filePath, net::Address ip, unsigned short port){	
	//open file
	FileWrapper file(filePath, CHUNK_SIZE);
	if (!file.Exists()) {
		throw CannotOpenFileException();
	}

	char name[_MAX_FNAME];
	char ext[_MAX_EXT];
	_splitpath_s(filePath.c_str(), NULL, 0, NULL, 0, name, _MAX_FNAME, ext, _MAX_EXT);
	char fileName[_MAX_FNAME + _MAX_EXT + 1];
	strcpy_s(fileName, sizeof(fileName), name);
	strcat_s(fileName, sizeof(fileName), ".");
	strcat_s(fileName, sizeof(fileName), ext);

	//initiate connection
	if (!connection.Start(port)){
		throw FailedToOpenSocketException();
	}
	connection.Connect(ip);

	//pair with sequenceNumber and chunk index of un-acked chunks
	vector<pair<unsigned int,_int64>> unAckedChunks;
	//todo: CALL PROGRESSCALLBACK WHEN FILE DONE
	bool connected = false;
	_int64 chunkIndex = 0;
	enum State {
		requestingToSend,
		sendingFile,
		transmissionComplete,
		finished
	};
	State state = requestingToSend;

	_int64 ackedChunkCount = 0;
	unsigned int transmissionCompleteSequence = 0;
	while (state != finished) {
		if (connection.ConnectFailed()) {
			throw ConnectFailedException();
		}
		
		if (!connected && connection.IsConnected()) {
			connected = true;
		}

		//lost connection
		if (connected && !connection.IsConnected()) {
			throw LostConnectionException();
		}

		//send
		switch (state){
		case requestingToSend: {

			char cmd[] = "REQUEST_SEND";
			_int64 fileSize = file.GetFileSize();
			int packetSize = sizeof(cmd) + sizeof(_int64) + (strlen(fileName) + 1);//+1 for null terminator
			unsigned char* packet =
				new unsigned char[packetSize];//+1 for null terminator
			memcpy(packet, cmd, sizeof(cmd));
			memcpy(packet + sizeof(cmd), &fileSize, sizeof(_int64));
			memcpy(packet + sizeof(cmd) + sizeof(_int64), fileName, strlen(fileName) + 1);//+1 for null terminator

			connection.SendPacket(packet, packetSize);

			delete(packet);
			break;
		}
		case sendingFile: {
			bool chunkSent = false;

			//send old chunk
			for (vector<int>::size_type i = 0; i < unAckedChunks.size(); i++) {
				unsigned int localSequence = connection.GetReliabilitySystem().GetLocalSequence();
				//if un acked chunk is too old to be acked
				if (overflowDiff(unAckedChunks[i].first, localSequence) > 33) {
					_int64 oldChunkIndex = unAckedChunks[i].second;

					//remove old
					unAckedChunks.erase(unAckedChunks.begin() + i);

					//add new
					unAckedChunks.push_back(pair<unsigned int, _int64>(
						connection.GetReliabilitySystem().GetLocalSequence(),
						oldChunkIndex
					));

					//resend
					char cmd[] = "CHUNK";
					unsigned char packet[CHUNK_SIZE + sizeof(_int64) + sizeof("CHUNK")];
					memcpy(packet, cmd, sizeof(cmd));
					memcpy(packet + sizeof(cmd), &oldChunkIndex, sizeof(_int64));
					file.GetChunk((char*)packet + sizeof(_int64) + sizeof("CHUNK"), oldChunkIndex);
					connection.SendPacket(packet, sizeof(packet));

					chunkSent = true;
					break;
				}
				if (chunkSent) {
					break;
				}
			}

			//send new chunk
			if (!chunkSent) {
				if (chunkIndex <= file.GetMaxChunk()) {
					//create packet
					char cmd[] = "CHUNK";
					unsigned char packet[CHUNK_SIZE + sizeof(_int64) + sizeof("CHUNK")];
					memcpy(packet, cmd, sizeof(cmd));
					memcpy(packet + sizeof(cmd), &chunkIndex, sizeof(_int64));
					file.GetChunk((char*)packet + sizeof(_int64) + sizeof("CHUNK"), chunkIndex);

					//add to unAckedChunks
					unAckedChunks.push_back(pair<unsigned int, _int64>(
						connection.GetReliabilitySystem().GetLocalSequence(),
						chunkIndex
					));

					//send
					connection.SendPacket(packet, sizeof(packet));
					chunkIndex++;
				} else {
					if (unAckedChunks.size() == 0) {
						//start waiting for server to ack transmission complete message
						state = transmissionComplete;
						transmissionCompleteSequence = connection.GetReliabilitySystem().GetLocalSequence();
						char cmd[] = "TRANSMISSION_COMPLETE";
						connection.SendPacket((unsigned char*)cmd, sizeof(cmd));
					}else {
						//send empty packet to maintain connection
						//connection.SendPacket(NULL, 0);//debug
						char emptyPacket[] = "empty";
						connection.SendPacket((unsigned char*)emptyPacket, sizeof(emptyPacket));
					}
				}
			}
			break;
		}
		case transmissionComplete: {
			//if cmd not acked for too long
			if (overflowDiff(transmissionCompleteSequence, connection.GetReliabilitySystem().GetLocalSequence()) > 33) {
				//resend
				transmissionCompleteSequence = connection.GetReliabilitySystem().GetLocalSequence();
				char cmd[] = "TRANSMISSION_COMPLETE";
				connection.SendPacket((unsigned char*)cmd, sizeof(cmd));
			}
			break;
		}
		}

		//recv
		unsigned char packet[100];//todo: change to max possible recv packet size
		while (connection.ReceivePacket(packet, sizeof(packet)) > 0) {

			//remove acked chunks from unAckedChunks
			unsigned int acked[33];
			int ackCount = 0;
			connection.GetReliabilitySystem().GetAcks(acked, ackCount);
			bool doneLoop = false;
			for (int i = 0; i < ackCount; i++) {
				for (vector<int>::size_type j = 0; j < unAckedChunks.size(); j++) {
					//if sequence numbers match
					if (acked[i] == unAckedChunks[j].first) {
						unAckedChunks.erase(unAckedChunks.begin() + j);
						ackedChunkCount++;
						doneLoop = true;
						break;
					}
				}
				if (doneLoop) {
					break;
				}
			}

			switch (state) {
			case requestingToSend: {
				//check if packet starts with request accept send command
				if (strcmp((const char*)packet, "ACCEPT_SEND") == 0) {
					state = sendingFile;
				}
				break;
			}
			case transmissionComplete: {
				//check if transmission complete message is acked
				for (int i = 0; i < ackCount; i++) {
					if (acked[i] == transmissionCompleteSequence) {
						state = finished;
						break;
					}
				}
				break;
			}
			}
		}

		//update
		connection.Update(sendInterval);
		net::wait(sendInterval);//todo: account for time taken to execute iteration of loop
	}

	cout << "transmission complete\n";
	connection.Stop();
	return file.GetFileSize();
}

//receivingFileCallback must be set first, todo: document exceptions
void FileSenderReceiver::RecvFile(unsigned short port, string savePath, Address sender){

	//open file
	FileWrapper file(savePath, CHUNK_SIZE, fstream::in | fstream::out | fstream::trunc | fstream::binary);
	if (!file.Exists()) {
		throw CannotOpenFileException();
	}

	//initiate connection
	if (!connection.Start(port)) {
		throw FailedToOpenSocketException();
	}

	connection.ListenFor(sender);

	_int64 fileSize = 0;
	enum State {
		awaitingSendRequest,
		receivingFile,
		finished
	};
	State state = awaitingSendRequest;
	bool connected = false;
	bool senderAcked = false;//true when sender acks that the transfer was accepted
	while (state != finished) {
		if (!connected && connection.IsConnected()) {
			connected = true;
		}

		//lost connection
		if (connected && !connection.IsConnected()) {
			throw LostConnectionException();
		}


		//send
		if (state == receivingFile && !senderAcked) {
			char cmd[] = "ACCEPT_SEND";
			connection.SendPacket((unsigned char*)cmd, sizeof(cmd));
		}else {
			//send empty packet to maintain connection
			//connection.SendPacket(NULL, 0);//debug
			char emptyPacket[] = "empty";
			connection.SendPacket((unsigned char*)emptyPacket, sizeof(emptyPacket));
		}
		
		//recv
		unsigned char packet[CHUNK_SIZE + sizeof(_int64) + sizeof("CHUNK")];//size of the largest pack(chunk packet)
		while (connection.ReceivePacket(packet, sizeof(packet)) > 0) {
			switch (state) {
			case awaitingSendRequest: {
				//check if packet starts with request send command
				if (strcmp((const char*)packet, "REQUEST_SEND") == 0) {
					fileSize = *(int64_t*)(packet + sizeof("REQUEST_SEND"));
					char* fileName = (char*)packet + sizeof("REQUEST_SEND") + sizeof(_int64);
					state = receivingFile;
					break;
				}
				break;
			}
			case receivingFile: {
				if (strcmp((const char*)packet, "CHUNK") == 0) {
					if (!senderAcked) {
						senderAcked = true;
					}

					_int64 chunkIndex = *(int64_t*)(packet + sizeof("CHUNK"));

					_int64 maxChunk = (fileSize - 1) / CHUNK_SIZE;
					//if last chunk
					if (chunkIndex == maxChunk) {
						file.WriteChunk((char*)packet + sizeof("chunk") + sizeof(chunkIndex), chunkIndex, fileSize%CHUNK_SIZE);//only write remaining size
					}else {
						file.WriteChunk((char*)packet + sizeof("chunk") + sizeof(chunkIndex), chunkIndex);
					}
					
				}
				else if (strcmp((const char*)packet, "TRANSMISSION_COMPLETE") == 0) {
					//send empty packet so sender can see the ack
					char emptyPacket[] = "empty";
					connection.SendPacket((unsigned char*)emptyPacket, sizeof(emptyPacket));
					state = finished;
				}
				break;
			}
			}
		}

		//update
		connection.Update(sendInterval);
		net::wait(sendInterval);//todo: account for time taken to execute iteration of loop
	}

	cout << "transmission complete\n";
	connection.Stop();
}

//returns the number that when added to a would give b accounting for overflow
unsigned int overflowDiff(unsigned int a, unsigned int b, unsigned int max) {
	if(a > b) {
		return (max - a) + b;//todo: test
	}else {
		return b - a;
	}
}