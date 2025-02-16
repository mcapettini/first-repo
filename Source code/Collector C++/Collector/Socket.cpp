/*
	Author:			Marco Capettini
	Content:		SOCKET PART
					Server that collects data from all ESPs and provides them to upper layers

	Team members:	Matteo Fornero, Fabio Carfì, Marco Capettini, Nicolò Chiapello
 */

//#include "pch.h" // Auto included by Visual Studio
#include "stdafx.h"

#include "Socket.h"

mutex m_stop, m_command;
condition_variable cv_stop, cv_command;

int GUI_intention = -1; // 0: receiving MAC to blink; 1: receiving the list of selected MACs
string MACtoBlink;
shared_ptr<BlockingQueue_Aggregator> BQ_A_ptr_forAutoSocket;
int N_ESP_forAutoSocket;
vector<string> boards_mac_forAutoSocket;

Socket::Socket(int N_ESP, vector<string> boards_mac, shared_ptr<BlockingQueue_Aggregator> BQ_A_ptr)
{
	this->BQ_A_ptr = BQ_A_ptr;
	this->N_ESP = N_ESP;
	this->boards_mac = boards_mac;
	this->autoSocket = 0;
	this->localMacClock = false;
	this->ListenSocket = INVALID_SOCKET;
	this->s.resize(N_ESP);
	this->autoS.resize(1);
	this->s_ESPSocket.resize(N_ESP);
}

Socket::Socket()
{
	this->autoSocket = 1;
	this->localMacClock = false;
	this->ListenSocket = INVALID_SOCKET;
}

void Socket::operator() ()
{
	cout << "Sono nell'operator con valore: " << autoSocket << endl << flush;
	Socket::startServer();
}

int __cdecl Socket::startServer()
{
	int iResult; // Will contain return value of winsock functions
	WSADATA wsaData; // Data structure filled by WSAStartup with details of the Windows Sockets implementation

	// Initialize Winsock (initiates use of the Winsock DLL by a process)
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData); // MAKEWORD(2, 2) tells winsock version: high-order byte -> minor version number; low-order byte -> major version number.
	if (iResult != 0) {
		stringstream ss;
		ss << "WSAStartup() failed with error : " << WSAGetLastError();
		Synchronizer::report_error(Synchronizer::WinsockBadInitialization, ss.str());
		return 1;
	}

	// The addrinfo structure is used by the getaddrinfo function to hold host address information
	struct addrinfo *result = NULL;
	struct addrinfo hints;

	// Set parameters in addrinfo structure
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve server address and port
	/*
		The functions getaddrinfo() and getnameinfo() convert domain names, hostnames, and IP addresses
		between human-readable text representations	and structured binary formats for the operating system's networking API.
		getaddrinfo(): "result" variable will hold a linked list of addrinfo structures containing response information,
		filled with the appropriate sockaddr_in in and also the port retrieved at the beginning (DEFAULT_PORT).
	*/
	iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
	if (iResult != 0) {
		WSACleanup();
		stringstream ss;
		ss << "getaddrinfo() failed with error: " << WSAGetLastError();
		Synchronizer::report_error(Synchronizer::WinsockBadInitialization, ss.str());
		return 1;
	}

	// Create a SOCKET for connecting to server
	ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (ListenSocket == INVALID_SOCKET) {
		freeaddrinfo(result);
		WSACleanup();
		stringstream ss;
		ss << "socket() failed with error : " << WSAGetLastError();
		Synchronizer::report_error(Synchronizer::WinsockBadInitialization, ss.str());
		return 1;
	}

	// Bind the IP address and port to a socket
	iResult = ::bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen); // :: is necessary if using namespace std
	if (iResult == SOCKET_ERROR) {
		freeaddrinfo(result);
		closesocket(ListenSocket);
		WSACleanup();
		stringstream ss;
		ss << "bind() failed with error: " << WSAGetLastError();
		Synchronizer::report_error(Synchronizer::WinsockBadInitialization, ss.str());
		return 1;
	}

	freeaddrinfo(result);

	// Tell Winsock the socket is for listening 
	iResult = listen(ListenSocket, SOMAXCONN); // That is 0x7fffffff
	if (iResult == SOCKET_ERROR) {
		closesocket(ListenSocket);
		WSACleanup();
		stringstream ss;
		ss << "listen failed with error: " << WSAGetLastError();
		Synchronizer::report_error(Synchronizer::WinsockBadInitialization, ss.str());
		return 1;
	}

	/*
		1. Some procedures to start Winsock properly and set the main TCP socket(s)
		2. Start a pseudo-infinite loop (stops when "somenone" tells to stop or some error occurs)
			3. Check if have to call autoServer.
			4. Check if have to call manageLocalMac() now or next cycle.
			5. For-loop to establish N_ESP connections/sockets
				6. Check if sockets must be new (perform an "accept") or can be old (re-uses the sockets of recAndPut() if possible), and this is done exchanging "ALIVE" messages.
				7. Receive MAC from ESP and tell to go to SYNCH if the MAC is ok, or to SLEEP if the MAC is not ok
				8. Receive a message from ESP that tells if the SYNCH was done correctly (NOSYNC: ESP was able to get the time from NTP server) or has to be done manually (SYNREQ: the ESP request manual synch)
				9. If has to be done manually (SYNREQ), send a bunch of timestamps.
			10. ESP are ready and so the server so tell ESP to start sniffing: send of GO\0 and close sockets.
			--- a minute later
			11. For-loop to accept N_ESP connections, which means that N_ESP are ready to send data.
			12. Create N_ESP threads each one doing the function recAndPut().
			13. Thread father waits all the threads.
			14. Repeat: go to 4.
		15. Stop.
	*/

	// Variables initialization
	int i;
	char bufAlive[6]; // To manage ALIVE/0 message

	s.resize(N_ESP); // resize() is used because you can't declare a variable[N_ESP] if N_ESP can vary
	for (i = 0; i < N_ESP; i++) { s[i] = INVALID_SOCKET; }

	bool sock_alive; // If false, socket (s[i]) can't be re-used (closed for some reason), so perform a new accept()
	vector<bool> synch_flag; // If false, synch isn't done yet, so try to synch again
	synch_flag.resize(N_ESP);

	int timeoutRecv = 20000; // Variable for timeout on recv() (20s)

	int countSynchFlag = 0; // To check if all esp are waiting for the go signal
	bool flagGOFailed = false; // In case of errors when we sent GO signal do not launch threads and skip immediately to next synchronization
	bool flagAcceptfailed = false; // In case of error on accepting (even only one) ESP connections, close all sockets and go back to synchronization
	uint8_t wifi_channel = 1;

	thread thrStop(&Socket::stopRoutine, this);

	// Server main loop, pseudo-infinite
	while (Synchronizer::_status != Synchronizer::alt) {
		try{
			if (autoSocket == 1) { // User request to enter auto-setup mode i.e. he has the possibility to make the ESPs blink
				if (serverAutoConfig()) {
					if (Synchronizer::_status != Synchronizer::alt) { // report_error not yet called
						stringstream ss;
						ss << "Error occurred during auto-configuration";
						Synchronizer::report_error(Synchronizer::SocketError, ss.str());
						thrStop.join(); cout << "Server powered off" << endl; WSACleanup();
						return 0;
					}
					else { // report_error already called, just exit loop and stop
						break;
					}
				}

				BQ_A_ptr = BQ_A_ptr_forAutoSocket;
				N_ESP = N_ESP_forAutoSocket;
				boards_mac = boards_mac_forAutoSocket;
				s_ESPSocket.resize(N_ESP);
				synch_flag.resize(N_ESP);
				countSynchFlag = N_ESP;

				autoSocket = 0;
			}
			else {
				// Check if have to call manageLocalMac() now or next time (2 minutes after, a part first time)
				if (localMacClock) {
					Socket::manageLocalMac();
					localMacClock = false;
				}
				else {
					localMacClock = true;
				}
				// Check if socket previously used with each esp is still valid, if not open new socket
				for (i = 0; i < N_ESP; i++) {
					synch_flag[i] = false;
					bool failed = false;
					while ((synch_flag[i] == false) && (Synchronizer::_status != Synchronizer::alt)) {
#if DEBUG
						cout << "Start synch ESP32 #" << i << endl;
#endif // DEBUG

						sock_alive = false;
						if (s[i] != INVALID_SOCKET) { // Checks if sockets is still alive (can be reused)
							iResult = send(s[i], "ALIVE\0", 6, 0);
							if (iResult == 6) {
								memset(bufAlive, '0', 6);
								iResult = recv(s[i], bufAlive, 6, 0);
								if (iResult == 6 && strncmp(bufAlive, "ALIVE\0", 6) == 0) {
									sock_alive = true;
								}
							}
						}
						if (sock_alive == false) {
							closesocket(s[i]); // Probabily has already been closed, just to be sure		
#if DEBUG
							cout << "Waiting on accept" << endl;
#endif // DEBUG
							s[i] = accept(ListenSocket, NULL, NULL); // Create a new socket if previous one is unusable
							if (s[i] == INVALID_SOCKET) {
#if DEBUG
								cout << "accept() on synch socket failed, trying again..." << endl;
#endif // DEBUG
								continue; // Restart the loop for this esp (or stop Server)
							}
						}

						// Set timeout for recv()
						iResult = setsockopt(s[i], SOL_SOCKET, SO_RCVTIMEO, (char *)&timeoutRecv, sizeof(timeoutRecv)); // SOL_SOCKET is the level (layer), SO_RCVTIMEO is the option
						if (iResult == SOCKET_ERROR) {
#if DEBUG
							cout << "setsockopt() for recv() before synch() failed with error code " << WSAGetLastError() << endl;
#endif // DEBUG
							closesocket(s[i]);
							continue; // Restart the loop for this esp
						}

						// Receive MAC from ESP and tell it to go to sleep or proceed with synch
						char buffer[18];
						memset(buffer, '0', 18);
						iResult = recv(s[i], buffer, 18, 0);
						if (iResult == 18) {
							if (find(boards_mac.begin(), boards_mac.end(), buffer) != boards_mac.end()) {
								// The ESP that is trying to connect it's one of the expected (its MAC is expected), accept connection
								iResult = send(s[i], "SYNCH\0", 6, 0);
								if (iResult != 6) {
#if DEBUG
									cout << "send of SYNCH\0 to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
									closesocket(s[i]);
									continue;
								}
							}
							else {
								// The ESP that is trying to connect it's not one of the expected (its MAC is not expected), refuse connection
#if DEBUG
								cout << "This ESP is not allowed, trying another accept..." << endl;
#endif // DEBUG
								iResult = send(s[i], "SLEEP\0", 6, 0);
								if (iResult != 6) {
#if DEBUG
									cout << "send of SLEEP\0 to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
									closesocket(s[i]);
									continue;
								}
								closesocket(s[i]);
								continue;
							}
						}
						else {
#if DEBUG
							cout << "recv() of MAC before synch failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
							closesocket(s[i]);
							continue;
						}

						char buffer_synch[7];
						memset(buffer_synch, '0', 7);
						iResult = recv(s[i], buffer_synch, 7, 0);
						if (iResult == 7 && strncmp(buffer_synch, "SYNREQ\0", 7) == 0) { // Check if an ESP was not able to get the time from NTP server 
							char buf[8];
							memset(buf, '0', 8);
							struct timeval tv;
							for (int j = 0; j < 20; j++) { // Esp was not able to get the time from ntp server, send a bunch of timestamps 
								gettimeofday(&tv); // Fill timeval with current time
								serialize_uint((unsigned char*)buf, tv.tv_sec, 4); // Put seconds in first 4 bytes
								serialize_uint((unsigned char*)(buf + 4), tv.tv_usec, 4); // Put microseconds in remaining 4 bytes
								iResult = send(s[i], buf, 8, 0);
								if (iResult != 8) {
#if DEBUG
									cout << "send of time to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
									closesocket(s[i]);
									failed = true;
									break;
								}
								Sleep((int)5000 / (N_ESP * 20)); // Try to sleep for some time (max 5 seconds always)
							}
						}
						else if (iResult == 7 && strncmp(buffer_synch, "NOSYNC\0", 7) == 0) {
								// The esp has already got the time from ntp server
						}
						else {
#if DEBUG
							cout << "recv failed in receiving time mode from esp: " << WSAGetLastError() << endl;
#endif // DEBUG
							closesocket(s[i]);
							continue; // Restart the loop for this esp (or stop Server)
						}

						if (failed == false) {
							synch_flag[i] = true; // This esp is ok, go on with the others while this one will wait for the go signal
							countSynchFlag++;
						}
						else {
							failed = false;
							continue;
						}
					}
				}
			}
			if (Synchronizer::_status != Synchronizer::alt) {}
			else {
				continue;
			}

			if (countSynchFlag == N_ESP) { // All esp are waiting for the go so tell them to start sniffing packets
				for (i = 0; i < N_ESP; i++) {
					char tmp[4] = "GO\0";
					tmp[3] = wifi_channel;
					cout << "Sending GO #" << i << endl;
					iResult = send(s[i], tmp, 4, 0);
					if (iResult != 4) {
#if DEBUG
						cout << "send of GO failed" << endl;
#endif // DEBUG
						flagGOFailed = true;
						closesocket(s[i]);
						break;
					}
					/*else {
#if DEBUG
						cout << "Synchronization of ESP #" << i << " done" << endl;
#endif // DEBUG
					}*/

				}
				for(i = 0; i < N_ESP; i++){
					char ack[1];
					recv(s[i], ack, 1, 0); // This recv is useful to wait ESP received MAC
					cout << "Synchronization of ESP #" << i << " really done" << endl;
				}
			}
			countSynchFlag = 0;
			for (i = 0; i < N_ESP; i++) {
				closesocket(s[i]); // Close all the sockets, they are not necessary anymore (in any case)
			}
			if (flagGOFailed == true) { // In case of errors when we sent GO signal do not launch threads and skip immediately to next synchronization
				flagGOFailed = false;
				continue;
			}

			for (i = 0; i < N_ESP; i++) {
				// Set timeout for accept()
				LPWSAPOLLFD pollfd = (LPWSAPOLLFD)malloc(sizeof *pollfd); // Vector of WSAPOLLFD struct: #dimension = #interested socket
				if (pollfd == NULL) {
					stringstream ss;
					ss << "Insufficient memory";
					Synchronizer::report_error(Synchronizer::MemoryError, ss.str());
					thrStop.join(); cout << "Server powered off" << endl; WSACleanup();
					return 1;
				}
				pollfd->fd = ListenSocket; // WSAPoll() will wait on ListenSocket
				pollfd->events = POLLRDNORM; // WSAPoll() will wait for a "read event"
				pollfd->revents = NULL; // WSAPoll() will store here the result of the recorded event
				int timeoutAcc;

				if (i == 0) {
					timeoutAcc = 70000;
#if DEBUG
					cout << "Waiting for data collected by all the ESP32\n";
#endif // DEBUG
				}
				else {
					timeoutAcc = 5000;
				}

				// Accept a client socket
				iResult = WSAPoll(pollfd, 1, timeoutAcc); // 1 is the #interested socket (in our case only ListenSocket)
				if (iResult == 0) {
#if DEBUG
					cout << "accept() waited too long - timeout expired - going back to synchronization" << endl;
#endif // DEBUG
					free(pollfd); pollfd = NULL;
					flagAcceptfailed = true;
					break;
				}
				else if (iResult > 0) {
					s_ESPSocket[i] = accept(ListenSocket, NULL, NULL);
					//cout << "Connection accepted\n" << endl;
					if (s_ESPSocket[i] == INVALID_SOCKET) {
#if DEBUG
						cout << "accept() failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
						closesocket(s_ESPSocket[i]);
						free(pollfd); pollfd = NULL;
						flagAcceptfailed = true;
						break;
					}
				}
				else if (iResult == SOCKET_ERROR) {
#if DEBUG
					cout << "WSAPoll() failed with error " << WSAGetLastError << endl;
#endif // DEBUG
					free(pollfd); pollfd = NULL;
					flagAcceptfailed = true;
					break;
				}
				free(pollfd); pollfd = NULL;
			}

			if (flagAcceptfailed) {
				for (i = 0; i < N_ESP; i++) {
					closesocket(s_ESPSocket[i]);
				}
				flagAcceptfailed = false;
				continue;
			}

			// Create N threads that receive data and put those data on Blocking Queue
			vector<thread> thr;
			thr.resize(N_ESP);
			for (i = 0; i < N_ESP; i++) {
				thr[i] = thread(&Socket::recAndPut, this, i); // Index "i" will be useful to save socket in right position of vector "s" in order to re-use it
			}
			// Wait all N threads, and repeat
			for (i = 0; i < N_ESP; i++) {
				thr[i].join();
			}
			/*
			Set all the copies of the socket used in data receiving to invalid, in this way the function stopServer can not worry about these sockets.
			In fact	these sockets can be already closed (due to a failure) or keep running but in this last case we have "s[]" that have the current
			information about the open socket, and stopServer can close it.
			*/
			s_ESPSocket.clear();
			s_ESPSocket.resize(N_ESP);

			wifi_channel = (wifi_channel % CHANNELS) + 1;
		}
		catch (const std::exception&) {
			stringstream ss;
			ss << "Error occurred in the socket";
			Synchronizer::report_error(Synchronizer::SocketError, ss.str());
		}
	}

	thrStop.join();

	cout << "Server powered off" << endl;
	closesocket(ListenSocket);
	WSACleanup();

	return 0;
}

int gettimeofday(struct timeval *tp)
{
	// Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
	// This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
	// until 00:00:00 January 1, 1970 
	static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

	SYSTEMTIME  system_time;
	FILETIME    file_time;
	uint64_t    time;

	GetSystemTime(&system_time);
	SystemTimeToFileTime(&system_time, &file_time);
	time = ((uint64_t)file_time.dwLowDateTime);
	time += ((uint64_t)file_time.dwHighDateTime) << 32;

	tp->tv_sec = (long)((time - EPOCH) / 10000000L);
	tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
	return 0;
}

void Socket::recAndPut(int index)
{
	/*
		Main receiving loop: (Packets are in the format: string "DATA"(4B), lenght(2B), data(32B), frame body(remaining B))
		> Set timeouts for blocking winsock call (recv() on ESPsocket)
		> Receive ESP MAC address (together with a message: SENDING)
		> Read number of packets to receive
			> Wait until you receive DATA
			> Receive lenght of packet
			> Receive packet (if you receive less than packet lenght receive again until the end)
			> Deserialize and extract fingerprint from frame body
			> Put in Blocking Queue
		> Record socket
	*/
	int iResult, iResultSend, countPkt = 0, macBoardLen = 18;
	unsigned int N_pkt; // Will contain #packets to receive from an ESP
	unsigned int L_pkt; // Will contain the lenght of one single packet that has to be received
	unsigned int L_received = 0; // Will contain the #bytes received of a packet, note that L_received <= L_pkt
	SOCKET ESPSocket = s_ESPSocket[index]; // Socket related to an ESP
	int timeoutRecv = 25000; // (20s)
	//char *recvBuf = NULL; // Will contain all or part of the data of a single packet 
	//char *pktBuf = NULL; // Useful to reconstruct the entire packet if recvBuf contains only part of a packet
	char recvInfo[3]; // Will store info about packets like: #packet to receive, the word "DATA" and the lenght of the packet
	string macBoard;
	char buffer[25]; // Will store the message containing the intention followe by the MAC of an ESP
	vector<char *> all_packets;

	try {
		// Set timeout for recv()
		iResult = setsockopt(ESPSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeoutRecv, sizeof(timeoutRecv)); // SOL_SOCKET is the level (layer), SO_RCVTIMEO is the option
		if (iResult == SOCKET_ERROR) {
#if DEBUG
			cout << "setsockopt() for recv() failed with error code " << WSAGetLastError() << endl;
#endif // DEBUG
			return;
		}

		// Read ESP's intention followed by its MAC address
		iResult = recv(ESPSocket, buffer, 25, 0);
		if (iResult == 25 && strncmp(buffer, "SENDING", 7) == 0) {
			// The ESP that is trying to connect it's one of the expected (its MAC is surely on of the expected), accept connection
			// No need to tell nothing, go on with reception
			macBoard = string(&buffer[7], &buffer[24]);
		}
		else {
			// Maybe the ESP has rebooted, or another non-expected ESP is trying to connect, in both case close socket
			// In this way that ESP will reboot and we return at the first accpet of the server so it's ok
#if DEBUG
			cout << "recv() failed in receiving ESP intentions+MAC with error: " << WSAGetLastError() << endl;
			cout << "Maybe the expected ESP has rebooted, or another non-expected ESP is trying to connect" << endl;
#endif // DEBUG
			closesocket(ESPSocket);
			return;
		}

#if DEBUG
		cout << "MAC of ESP number " << index << ": " << macBoard << endl;
#endif // DEBUG
		
		string filename = macBoard;
		replace(filename.begin(), filename.end(), ':', '-');
		filename.append(".txt");
		ofstream fileout;
		fileout.open(filename, ios::app);
		
		iResult = recv(ESPSocket, recvInfo, 2, 0); // Read number of packet to receive
		if (iResult != 2) {
#if DEBUG
			cout << "recv() failed on receiving #packets with error: " << WSAGetLastError() << endl;
#endif // DEBUG
			closesocket(ESPSocket);
			return;
		}
		N_pkt = deserialize_uint((unsigned char*)recvInfo, 2);
		// If no packets were collected by the ESP, jump immediately, otherwise do all the stuff
		if (N_pkt == 0) {
#if DEBUG
			cout << "No packets collected by the ESP, move on with next iteration...";
#endif // DEBUG
		}
		else { // Main receiving "loop"
			while (countPkt != N_pkt) {
				char data[4] = "000";
				do {
					// Receiving of "DATA" (4B)
					iResult = recv(ESPSocket, recvInfo, 1, 0);
					if (iResult == 0) {
						cout << "Connection closed by ESP" << endl;
						closesocket(ESPSocket);
						return;
					}
					else if (iResult != 1) {
						cout << "recv() on receiving DATA field failed with error: " << WSAGetLastError() << endl;
						closesocket(ESPSocket);
						return;
					}
					//cout << recvInfo[0];
					data[0] = data[1];
					data[1] = data[2];
					data[2] = data[3];
					data[3] = recvInfo[0];
				} while (strncmp(data, "DATA", 4) != 0);
				//cout << endl;

				// Receiving the lenght (2B) of the packet to be received
				iResult = recv(ESPSocket, recvInfo, 2, 0);
				if (iResult == 0) {
#if DEBUG
					cout << "Connection closed by ESP" << endl;
#endif // DEBUG
					closesocket(ESPSocket);
					return;
				}
				else if (iResult != 2) {
#if DEBUG
					cout << "recv() on receiving lenght field failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
					closesocket(ESPSocket);
					return;
				}
				L_pkt = deserialize_uint((unsigned char*)recvInfo, 2);

				smart_ptr recvBuf{ L_pkt };
				smart_ptr pktBuf{ L_pkt };

				L_received = 0;

				// Receiving of a packet
				while (L_received < L_pkt) {
					iResult = recv(ESPSocket, recvBuf.get(), (L_pkt - L_received), 0);
					if (iResult == L_pkt) {
						memcpy_s(pktBuf.get(), L_pkt, recvBuf.get(), L_pkt);
						break;
					}
					if (iResult == 0) {
#if DEBUG
						cout << "Connection closed by ESP" << endl;
#endif // DEBUG
						closesocket(ESPSocket);
						return;
					}
					if (iResult < L_pkt) { // Manage fragmentation
						memcpy_s(pktBuf.get(), (L_pkt - L_received), recvBuf.get(), iResult);
						pktBuf + iResult; // Move the pointer onwards to append next data
						L_received = L_received + iResult;
						if (L_received == L_pkt) { // Fragments recomposed
							pktBuf - L_pkt; // Move the pointer backwards to have the proper data
						}
					}
					else {
#if DEBUG
						cout << "recv() on receiving packets failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
						closesocket(ESPSocket);
						return;
					}
				}
				// A packet has been received
				
				ESPpacket pkt = deserialize_data(pktBuf.get());

				string SSID;
				uint32_t hashedFrameBody;

				pktBuf + 32; // Jump to frame body

				//for (int i = 0; i < (L_pkt-32); i++) { printf("%02x", pktBuf[i]); }	printf("\n");

				hashedFrameBody = Socket::parser((unsigned char*)pktBuf.get(), (L_pkt - 32), &SSID);
				if (hashedFrameBody == 0) {
					pktBuf - 32;
					throw std::exception("Parser problem");
				}

				if (SSID.empty()) {
					SSID = "broadcast";
				}
#if DEBUG
				//cout << "ESP " << macBoard << " -> " << "MAC: " << pkt.get_MAC().c_str() << " | SSID: " << SSID.c_str() << " | RSSI: " << static_cast<int16_t>(pkt.get_rssi()) << " | Seq.num: " << pkt.get_seqnum() << " | Timestamp: " << pkt.get_timestamp() << " | Hash: " << pkt.get_hash() << " | HashedFrameBody: " << hashedFrameBody << endl;
#endif // DEBUG
				fileout << "ESP " << macBoard << " -> " << "MAC: " << pkt.get_MAC().c_str() << " | SSID: " << SSID.c_str() << " | RSSI: " << static_cast<int16_t>(pkt.get_rssi()) << " | Seq.num: " << pkt.get_seqnum() << " | Timestamp: " << pkt.get_timestamp() << " | Hash: " << pkt.get_hash() << " | HashedFrameBody: " << hashedFrameBody << endl;
				
				
				putInBQ(pkt, macBoard, SSID, hashedFrameBody); // Put data into BQ for upper layers

				countPkt++;
				pktBuf - 32; // Go back to the start of the vector in order to free it properly
			}
#if DEBUG
			cout << "Number of packet received: " << countPkt << endl;
#endif // DEBUG

		}

#if DEBUG
		cout << "\n--------------------------------------------------------------------------------------\n";
#endif // DEBUG

		fileout.close();
		// If you are here, packet reception is completed succesfully, so don't close socket and try to re-use it for next synch 
		s[index] = ESPSocket;

	}
	catch (const std::exception &)
	{
		stringstream ss;
		ss << "Error occurred in the socket";
		Synchronizer::report_error(Synchronizer::SocketError, ss.str());
	}


	return;
}

void Socket::putInBQ(ESPpacket pkt, string macBoard, string SSID, uint32_t fingerprint)
{
	/*
		While putting packet into the BQ, they have to change type (from ESPpacket to Detection), so:
		fill the object (of type Detecion) with all the packet data except for sequence number and SSID.
		If the packet has a local MAC add also the sequence number, the SSID and the fingerprint.
	*/
	char c = pkt.get_MAC().at(1);
	Detection detec(pkt.get_hash(), macBoard, pkt.get_MAC(), std::chrono::milliseconds{ pkt.get_timestamp() }, static_cast<int16_t>(pkt.get_rssi()));

	if (c == '2' || c == '3' || c == '6' || c == '7' || c == 'a' || c == 'b' || c == 'e' || c == 'f') { // Local MAC
		detec.set_private_MACaddress(pkt.get_seqnum(), SSID, fingerprint);
	}

	//Put detec on BQ
	BQ_A_ptr->insert(detec);

	return;
}

uint32_t Socket::parser(unsigned char *buffer, size_t len, string *SSID)
{
	/*
		Here we analyze the TLV structure.
		Every time we encounter an interesting elementID we take its Value (or just the elementID), interpret it as a sequence of char, and then append it to "compactedFrameBody string.
		In this way, at the end, this string will contain a sort of string/fingerprint non human-readable, hopefully identical in packets of same device.
	*/

	uint16_t index = 0, i;
	unsigned int elementLength = 0;
	unsigned int elementID = 0;
	bool flagVendor = false; // If true the first vendor field has already been taken, so do not take the others	
	string compactedFrameBody;

	try {
		while (index < len) {
			elementID = (unsigned int)buffer[index++]; // read type and increment index
			elementLength = buffer[index++]; // read length and increment index

			if (elementLength > 0) {
				switch (elementID) { // See PR_frameIDs.h to understand fields
				case IE_SSID:
				{
					buffer += index;
					std::string tmp((char*)buffer, elementLength);
					SSID->assign(tmp);
					buffer -= index;
					break;
				}
				case IE_SUPPORTED_RATES:
					compactedFrameBody.append(buffer[index], elementLength);
					break;
				case IE_HT_CAPABILITIES:
					compactedFrameBody.append(to_string(elementID));
					break;
				case IE_EXTENDED_SUPPORTED_RATES:
					compactedFrameBody.append(buffer[index], elementLength);
					break;
				case IE_EXTENDED_CAPABILITIES:
					compactedFrameBody.append(to_string(elementID));
					break;
				case IE_VENDOR_SPECIFIC:
					if (flagVendor == false) {
						compactedFrameBody.append(buffer[index], elementLength);
						flagVendor = true;
					}
					break;
				default:
					//cout << "Found incompatible ElementID in PROBE REQUEST frame body." << endl;
					break;
				}
				index = index + elementLength;
			}
		}

		// DJB2 hash algorithmche
		unsigned char* str = (unsigned char*)compactedFrameBody.c_str();
		unsigned long hash = 5381;
		int c;
		while (c = *str++) {
			hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
		}
		uint32_t hashedFrameBody = hash;

		return hashedFrameBody;
	}
	catch (const std::exception&) {
		return 0;
	}
}

int Socket::serverAutoConfig()
{
	/*
		1 Perform an "accept" after another until an accept waits more then "timeout" seconds, if so it means that all the surrounding ESPs has performed an accept and no more ESP are trying to connect (because you already have accepted them all).	
		2 Receive MAC from all the ESP.
		3 Send all these MAC to GUI, then wait for GUI instructions (enter "GUI_command_routine()" and waits).
		4 If GUI want to blink an ESP (particular MAC), then send a BLINK message to that ESP.
		5 If GUI started the system a list of chosen MAC is received, so then send a SLEEP message to all the ESP with a non-chosen MAC and a SYNCH message to all the ESP with a chosen MAC.
		6 Perform synchronization.
		7 Return to regular server.
	*/

	int countAcc = 0; // Number of accepted socket
	int iResult;
	int timeoutAcc;
	int timeoutRecv = 5000;

	vector<string> MACs;

	vector<bool> synch_flag; // If false, synch isn't done yet, so try to synch again
	synch_flag.resize(30);

	// Set timeout for accept()
	LPWSAPOLLFD pollfd = (LPWSAPOLLFD)malloc(sizeof *pollfd); // Vector of WSAPOLLFD struct: #dimension = #interested socket
	if (pollfd == NULL) { return 1; }
	pollfd->fd = ListenSocket; // WSAPoll() will wait on ListenSocket
	pollfd->events = POLLRDNORM; // WSAPoll() will wait for a "read event"
	pollfd->revents = NULL; // WSAPoll() will store here the result of the recorded event

	while (Synchronizer::_status != Synchronizer::alt) {
#if DEBUG
		cout << "Waiting on accept #" << countAcc << endl << flush;
#endif // DEBUG
		if (countAcc == 0) { timeoutAcc = 10000; }
		else { timeoutAcc = 5000; }

		// Accept a client socket
		iResult = WSAPoll(pollfd, 1, timeoutAcc); // 1 is the #interested socket (in our case only ListenSocket)
		if (iResult == 0) {
			if (countAcc == 0) { 
				free(pollfd); pollfd = NULL;
				cout << "No ESP found in " << timeoutAcc << " milliseconds" << endl;
				return 1;
			}
#if DEBUG
			cout << "accept() waited too long - timeout expired - ready to receive MACs from ESPs" << endl << flush;
#endif // DEBUG
			break;
		}
		else if (iResult > 0) {
			SOCKET skt = accept(ListenSocket, NULL, NULL);
			if (skt == INVALID_SOCKET) {
#if DEBUG
				cout << "accept() failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
				closesocket(skt);
				continue;
			}
			autoS.push_back(skt);
		}
		else if (iResult == SOCKET_ERROR) {
#if DEBUG
			cout << "WSAPoll() failed with error " << WSAGetLastError << endl;
#endif // DEBUG
			free(pollfd); pollfd = NULL;
			cout << "WSAPoll() failed in serverAutoConfig" << endl; 
			return 1;
		}
		
		countAcc++;
	}

	// Receive MAC from an ESP, if something goes wrong (ESP rebooted), stop the system
	for (int i = 0; i < countAcc; i++) {
		iResult = setsockopt(autoS[i], SOL_SOCKET, SO_RCVTIMEO, (char *)&timeoutRecv, sizeof(timeoutRecv)); // SOL_SOCKET is the level (layer), SO_RCVTIMEO is the option
		if (iResult == SOCKET_ERROR) {
#if DEBUG
			cout << "setsockopt() for recv() of MAC failed with error code " << WSAGetLastError() << endl;
#endif // DEBUG
			closesocket(autoS[i]);
			return 1;
		}

		char MAC[18];
		memset(MAC, '0', 18);
		iResult = recv(autoS[i], MAC, 18, 0);
		if (iResult == 18) {
			if (strncmp(MAC, "SENDING", 7) == 0) {
				cout << "ATTENTION: You have started the auto configuration while ESPs are sending data, please retry..." << endl;
				return 1;
			}
			MACs.push_back(string(&MAC[0], &MAC[17]));
		}
		else {
			cout << "Timeout expired or error occurred or ESP closed the socket in serverAutoConfig" << endl;
			return 1;
		}
	}
	
	free(pollfd); pollfd = NULL;

	vector<string>::iterator it_MAC_i, it_MAC_j;
	vector<SOCKET>::iterator it_SOC;

	// If an ESP rebooted during the "accepts-cycle" you can have more than one socket-mac pair related to the same mac, so remove duplicates and mantain only the last occurrency
	//for (it_MAC_i = MACs.begin(), it_SOC = autoS.begin(); it_MAC_i < MACs.end(); it_MAC_i++, it_SOC++) { cout << *it_MAC_i << " " << *it_SOC << endl; }
	for (it_MAC_i = MACs.begin(), it_SOC = autoS.begin(); it_MAC_i < MACs.end(); it_MAC_i++, it_SOC++) {
		for (it_MAC_j = MACs.begin(); it_MAC_j < MACs.end(); it_MAC_j++) {
			if (it_MAC_i != it_MAC_j) {
				if (strcmp((*it_MAC_i).c_str(), (*it_MAC_j).c_str()) == 0) {
					MACs.erase(it_MAC_i);
					autoS.erase(it_SOC);
					it_MAC_i = MACs.begin();
					it_MAC_j = MACs.begin();
					it_SOC = autoS.begin();
				}
			}
		}
	}
	//cout << endl;
	//for (it_MAC_i = MACs.begin(), it_SOC = autoS.begin(); it_MAC_i < MACs.end(); it_MAC_i++, it_SOC++) { cout << *it_MAC_i << " " << *it_SOC << endl; }

	// Send to the GUI the list of MACs heard
	Synchronizer::declare_present_boards(MACs);
	// Wait for GUI instructions
	while (GUI_intention != 1) {
		if (Synchronizer::_status == Synchronizer::alt) { // Check if there are problems before going to wait
			return 1;
		}
		if (GUI_command_routine(MACs)) { return 1; }
	}

	timeoutRecv = 20000;
	// Perform synchronization
	for (int i = 0; i < N_ESP_forAutoSocket; i++) {

		iResult = setsockopt(s[i], SOL_SOCKET, SO_RCVTIMEO, (char *)&timeoutRecv, sizeof(timeoutRecv)); // SOL_SOCKET is the level (layer), SO_RCVTIMEO is the option
		if (iResult == SOCKET_ERROR) {
#if DEBUG
			cout << "setsockopt() for recv() of on synch failed with error code " << WSAGetLastError() << endl;
#endif // DEBUG
			closesocket(autoS[i]);
			return 1;
		}

		char buffer_synch[7];
		memset(buffer_synch, '0', 7);
		iResult = recv(s[i], buffer_synch, 7, 0);
		if (iResult == 7 && strncmp(buffer_synch, "SYNREQ\0", 7) == 0) { // Check if an ESP was not able to get the time from NTP server 
			char buf[8];
			memset(buf, '0', 8);
			struct timeval tv;
			for (int j = 0; j < 20; j++) { // Esp was not able to get the time from ntp server, send a bunch of timestamps 
				gettimeofday(&tv); // Fill timeval with current time
				serialize_uint((unsigned char*)buf, tv.tv_sec, 4); // Put seconds in first 4 bytes
				serialize_uint((unsigned char*)(buf + 4), tv.tv_usec, 4); // Put microseconds in remaining 4 bytes
				iResult = send(s[i], buf, 8, 0);
				if (iResult != 8) {
#if DEBUG
					cout << "send of time to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
					closesocket(s[i]);
					return 1;
				}
				Sleep((int)5000 / (N_ESP_forAutoSocket * 20)); // Try to sleep for some time (max 5 seconds always)
			}
		}
		else if (iResult == 7 && strncmp(buffer_synch, "NOSYNC\0", 7) == 0) {
			// The esp has already got the time from ntp server
		}
		else {
#if DEBUG
			cout << "recv failed in receiving time mode from esp (maybe due to timeout-expiration): " << WSAGetLastError() << endl;
#endif // DEBUG
			closesocket(s[i]);
			return 1;
		}
	}
	
	// Return to the real server, send the GO, receive packets and restart the real server loop
	return 0;
}

int Socket::GUI_command_routine(vector<string> MACs)
{
	unique_lock<mutex> ul(m_command);
	int iResult;
	
	//cout << "Going on waiting" << endl << flush;
	cv_command.wait(ul);
	//cout << "MAC TO BLINK: " << MACtoBlink << endl << flush;
	//cout << "GUI_INTENTION: " << GUI_intention << endl << flush;
	if (GUI_intention == 0) {
		auto pos = find(MACs.begin(), MACs.end(), MACtoBlink) - MACs.begin();
		
		iResult = send(autoS[pos], "BLINK\0", 6, 0);
		if (iResult != 6) {
#if DEBUG
			cout << "send of BLINK to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
			closesocket(autoS[pos]);
			return 1;
		}
	}
	else if (GUI_intention == 1) {
		vector<string>::iterator it_MACs, it_chosenMACs;
		int i = 0, j;

		// Scan the list of all listened MACs to find the MACs that correspond to the MACs chosen by the user
		// When you find each one, save (the position in the list and) the socket related to it.
		for (it_MACs = MACs.begin(), j=0; it_MACs != MACs.end(); it_MACs++, j++) {
			auto pos = find(boards_mac_forAutoSocket.begin(), boards_mac_forAutoSocket.end(), *it_MACs) - boards_mac_forAutoSocket.begin();

			if (pos < boards_mac_forAutoSocket.size()) { // The considered MAC is among the chosen, take its index and tell the related socket/ESP to go to synch 
				iResult = send(autoS[j], "SYNCH\0", 6, 0);
				if (iResult != 6) {
#if DEBUG
					cout << "send of SYNCH to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
					closesocket(autoS[pos]);
					return 1;
				}

				s.push_back(autoS[j]);
				i++;
			}
			else { // The considered MAC is not among the chosen, take its index and tell the related socket/ESP to sleep
				iResult = send(autoS[j], "SLEEP\0", 6, 0);
				if (iResult != 6) {
#if DEBUG
					cout << "send of SLEEP to esp failed with error: " << WSAGetLastError() << endl;
#endif // DEBUG
					closesocket(autoS[pos]);
					return 1;
				}
			}
		}
	}
	else if (GUI_intention == -1) {
		cout << "User decided to go back, stop." << endl;
		return 1;
	}
	
	return 0;
}

void Socket::Blink(string MAC)
{
	unique_lock<mutex> ul(m_command);

	//cout << "Lock taken" << endl << flush;

	GUI_intention = 0;
	MACtoBlink = MAC;

	cv_command.notify_all();
}

void Socket::concludeAutoSetup(int N_ESP, vector<string> boards_mac, shared_ptr<BlockingQueue_Aggregator> BQ_A_ptr)
{
	unique_lock<mutex> ul(m_command);

	GUI_intention = 1;
	BQ_A_ptr_forAutoSocket = BQ_A_ptr;
	N_ESP_forAutoSocket = N_ESP;
	boards_mac_forAutoSocket = boards_mac;

	cv_command.notify_all();
}

void Socket::stopServer()
{
	unique_lock<mutex> ul(m_stop);

	cv_stop.notify_all();

	return;
}

void Socket::stopRoutine()
{
	unique_lock<mutex> ul(m_stop);

	cv_stop.wait(ul);

	Synchronizer::_status = Synchronizer::alt;

	closesocket(ListenSocket);
	for (int i = 0; i < s.size(); i++) {
		closesocket(s[i]);
		if (s_ESPSocket.size() > 0) {
			closesocket(s_ESPSocket[i]);
		}
	}
	for (int i = 0; i < autoS.size(); i++) {
		closesocket(autoS[i]);
	}

	GUI_intention = -1;
	boards_mac_forAutoSocket.clear();

	cv_command.notify_all();

	return;
}
