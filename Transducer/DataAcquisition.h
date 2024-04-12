// SEP400 - ASSIGNMENT 2
// DataAcqusition.h - Contains all the header files for the Seismic Data Unit.
// Seismic Data Acquisition Unit
// ABHI PATEL - apatel477@myseneca.ca
// NEEL MAHIMKAR - nmahimkar@myseneca.ca
#ifndef DATAACQUISITION_H_
#define DATAACQUISITION_H_

#include "SeismicData.h"
#include <sys/shm.h>
#include <sys/types.h>
#include <semaphore.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <queue>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/ip.h>     
#include <arpa/inet.h>  
#include <pthread.h>
#include <map>
#include <set>
#include <algorithm>
#include <sstream>
using namespace std;

static void signalHandler(int sigint);

class DataAcquisition {
    // Struct to store a data packet
    struct DataPacket {
        uint8_t packetNo;      // Packet number
        uint16_t packetLen;    // Length of the data in the packet
        string data;           // Seismic data as a string
    };

    // Struct to store subscriber information
    struct Subscriber {
        int port;                               // Subscriber's port number
        char IP_addr[INET_ADDRSTRLEN];          // Subscriber's IP address
        char username[BUF2_LEN];                // Subscriber's username
    };


    bool is_running;                             // Flag to control the execution of threads
    pthread_mutex_t lock_x;                      // Mutex to protect shared data structures
    queue<DataPacket> packetQueue;              // Queue to store data packets
    map<string, Subscriber> subscribers;        // Map to store subscribers' information
    map<string, int> grey_list;                 // Map to store grey-listed clients
    map<string, Subscriber> black_list;         // Map to store black-listed clients
    sem_t *sem_id1;                              // Semaphore for shared memory synchronization
    key_t  ShmKey;                               // Shared memory key
    int    ShmID, sockfd;                        // Shared memory ID and socket file descriptor
    struct SeismicMemory *ShmPTR;               // Pointer to the shared memory
    pthread_t rd_tid, wr_tid;                   // Read and write threads' IDs


    // Authenticates a client based on the provided message, client address, and socket
    void authenticate(char cl_msg[BUF_LEN], struct sockaddr_in *cl_addr, int sockfd);
    // Checks the return value of a socket operation and handles errors
    void check(int);
    // Adds a client to the grey list and updates the black list if necessary
    void AddToGreyList(string key, Subscriber &sub);
    // Sets up the shared memory
    void setupSharedMemory();
    // Sets up the signal handler for catching signals
    void setupSignalHandler();
    // Sets up the socket for communication with data centers
    void setupSocket();
    // Creates the read and write threads
    void createThreads();
    // Reads seismic data from the shared memory
    void readMemory();

    public:
    //Default Constructor
    DataAcquisition();
    // Singleton instance
    static DataAcquisition* instance;
    // Receive thread function
    static void* recv_func(void *arg);
    // Send thread function
    static void* send_func(void *arg);
    // Runs the data acquisition process
    void run();
    // Shuts down the data acquisition process and cleans up resources
    void shutdown();
    // Exits gracefully with an error message
    void gracefulExit(const string& errorMessage);
};


#endif