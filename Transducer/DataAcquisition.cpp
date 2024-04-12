// SEP400 - ASSIGNMENT 2
// DataAcqusition.cpp - Function definiton s for the data acquistion unit
// Seismic Data Acquisition Unit
// ABHI PATEL - apatel477@myseneca.ca
// NEEL MAHIMKAR - nmahimkar@myseneca.ca
#include "DataAcquisition.h"

using namespace std;

DataAcquisition* DataAcquisition::instance = nullptr;

static void signalHandler(int signum) {
    switch (signum) {
        case SIGINT:
            // TODO: SHUTDOWN
            cout << "SHUTTING DATA ACQUISITION UNIT..." << endl;
            if (DataAcquisition::instance != nullptr) { // Check if the instance is not a nullptr before calling shutdown
                DataAcquisition::instance->shutdown();
            }
            break;
    }
}


// Constructor for the DataAcquisition class
DataAcquisition::DataAcquisition() {
    is_running = false;
    ShmPTR = nullptr;
    // Set the singleton instance pointer to this
    DataAcquisition::instance = this;
    signal(SIGINT, signalHandler); // Register the signal handler for SIGINT
}


// Function to set up the signal handler for catching signals like SIGINT
void DataAcquisition::setupSignalHandler() {
    struct sigaction action;
    action.sa_handler = signalHandler;  // Assign the signal handler function
    sigemptyset(&action.sa_mask);       // Clear the signal mask
    action.sa_flags = 0;                // Set flags to 0
    // Register the signal handler for SIGINT with the specified action
    sigaction(SIGINT, &action, NULL);
}


// Function to set up shared memory for inter-process communication
void DataAcquisition::setupSharedMemory() {
    // Set up shared memory key
    ShmKey = ftok(MEMNAME, 65);
    // Get shared memory ID with read-write permissions
    ShmID = shmget(ShmKey, sizeof(struct SeismicMemory), IPC_CREAT | 0666); //rw-rw-rw
    if (ShmID < 0) {
        cout << "[ShmID] Error: " << strerror(errno) << endl;
        exit(-1);
    }

    // Attach shared memory to the process address space
    ShmPTR = (struct SeismicMemory*) shmat(ShmID, NULL, 0);
    if (ShmPTR == (void *)-1) {
        cout << "[ShmPTR] Error: " << strerror(errno) << endl;
        exit(-1);
    }

    // Set up semaphore for synchronization
    sem_id1 = sem_open(SEMNAME, O_CREAT, SEM_PERMS, 0);

    if (sem_id1 == SEM_FAILED) {
        cout << "[ShmPTR] Error: " << strerror(errno) << endl;
        exit(-1);
    }

    // Set the is_running flag to true
    is_running = true;
}


void DataAcquisition::setupSocket() {
    struct sockaddr_in sv_addr;
    // Create a non-blocking UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sockfd == -1) {
        cerr << "Error creating socket: " << strerror(errno) << endl;
        exit(-1);
    }
  // Initialize server address structure
    memset(&sv_addr, 0, sizeof(sv_addr));
    sv_addr.sin_family = AF_INET;
    sv_addr.sin_port = htons(1153);
    check(inet_pton(AF_INET, "127.0.0.1", &sv_addr.sin_addr));
 
   // Bind the socket to the server address
    check(bind(sockfd, (struct sockaddr*)&sv_addr, sizeof(sv_addr)));
}


// Function to create read and write threads for data communication
void DataAcquisition::createThreads() {
    // Initialize the mutex lock
    pthread_mutex_init(&lock_x, NULL);
    int ret;

    // Create the receive thread
    ret = pthread_create(&rd_tid, NULL, recv_func, this);
    if (ret != 0) {
        gracefulExit("Error: Failed to create receive thread.");
    }

    // Create the send thread
    ret = pthread_create(&wr_tid, NULL, send_func, this);
    if (ret != 0) {
        gracefulExit("Error: Failed to create send thread.");
    }
}


// Function to read data from shared memory and add iterator to the packet queue
void DataAcquisition::readMemory() {
    int dataIdx = 0;
    cout << "*** READING shared memory ***" << endl; 
    while (is_running) {
        for (int i = 0; i < NUM_DATA; ++i) {
        // Check if the data has been written to shared memory
        if (ShmPTR->seismicData[i].status == WRITTEN) {
            // Create a new DataPacket and fill iterator with data from shared memory
            struct DataPacket packet;
            sem_wait(sem_id1);
            packet.data = string(ShmPTR->seismicData[dataIdx].data);
            packet.packetLen = ShmPTR->seismicData[dataIdx].packetLen;
            packet.packetNo = uint8_t(ShmPTR->packetNo);
            
            // Mark the shared memory data as read
            ShmPTR->seismicData[dataIdx].status = READ;
            sem_post(sem_id1);

            // Add the new packet to the queue
            pthread_mutex_lock(&lock_x);
            packetQueue.push(packet);
            pthread_mutex_unlock(&lock_x);
            }   
        }
        sleep(1);
    }
}


// Receive thread function that handles authentication and communication with clients
void* DataAcquisition::recv_func(void *arg) {
    cout << "*** STARTING UP Receive Thread ***" << endl; 
    DataAcquisition* instance = (DataAcquisition*)arg;
    int ret = 0;
    struct sockaddr_in client_addr;  
    socklen_t client_addr_len = sizeof(client_addr);
    char IP_addr[INET_ADDRSTRLEN];
    int port;
    char buf[BUF_LEN];
    memset(buf, 0, BUF_LEN);

    while (instance->is_running) {
        // Receive messages from clients
        memset(&client_addr, 0, sizeof(client_addr));
        ret = recvfrom(instance->sockfd, buf, BUF_LEN, 0, (struct sockaddr*) &client_addr, &client_addr_len);
    
        // If a message is received, authenticate the client and process the message
        if (ret > 0){
            memset(IP_addr, 0, INET_ADDRSTRLEN);
            inet_ntop(AF_INET, &(client_addr.sin_addr), IP_addr, INET_ADDRSTRLEN);
            port = ntohs(client_addr.sin_port);
            string key = string(IP_addr) + ":" + to_string(port);

            // If the client is not in the blacklist, authenticate the client
            if (instance->black_list.find(key) == instance->black_list.end()) {
                instance->authenticate(buf, &client_addr, instance->sockfd); 
            }
            memset(buf, 0, BUF_LEN);
        } 
    }
    cout << "*** QUITTING Receive Thread ***" << endl; 
    pthread_exit(NULL);
}


// Send thread function that sends data packets to authenticated clients
void* DataAcquisition::send_func(void *arg) {
    DataAcquisition* instance = (DataAcquisition*)arg;
    cout << "*** STARTING UP Send Thread ***" << endl; 
    struct sockaddr_in cl_addr;
    DataPacket *packet;
    map<string, Subscriber> subscribers;
    while(instance->is_running) {
   
        if (!instance->packetQueue.empty()) {
            cout << "DataPacket.size(): " << instance->packetQueue.size() << " client.size(): " << instance->subscribers.size() <<  endl;
            packet = &(instance->packetQueue.front());

            //NOTE: making a local copy of the subscribers since sendto could block (no mutexing)
            pthread_mutex_lock(&(instance->lock_x));
            subscribers =  instance->subscribers;
            pthread_mutex_unlock(&(instance->lock_x));

            for (auto iterator = instance->subscribers.begin(); iterator != instance->subscribers.end(); iterator++) {
  
                memset(&cl_addr, 0, sizeof(cl_addr));
                cl_addr.sin_family = AF_INET;
                cl_addr.sin_port = htons(iterator->second.port);
                instance->check(inet_pton(AF_INET, iterator->second.IP_addr, &cl_addr.sin_addr));
               
            // Verifying IP and PORT for debugging purposes
                char IP_addr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(cl_addr.sin_addr), IP_addr, INET_ADDRSTRLEN);

                cout << "send_func: " << IP_addr << ":" << ntohs(cl_addr.sin_port) << endl;
            // Prepare the buffer with packet data and additional header information
                char buf[BUF_LEN+3];
                buf[0] = packet->packetNo & 0xFF;
                buf[1] = (packet->packetLen >> 8) & 0XFF;
                buf[2] = packet->packetLen & 0xFF;
                memcpy(buf + 3, packet->data.c_str(), packet->packetLen);

                sendto(instance->sockfd, (char *)buf, BUF_LEN+3, 0,(struct sockaddr *)&cl_addr, sizeof(cl_addr));
          
            }
       
            pthread_mutex_lock(&instance->lock_x);
            instance->packetQueue.pop();// Pop the packet from the queue after it has been sent to all subscribers
            pthread_mutex_unlock(&instance->lock_x);
        }
        
        sleep(1);
    }

    cout << "*** QUITTING Receive Thread ***" << endl; 
    pthread_exit(NULL);
}


//Function to chekc for errors
void DataAcquisition::check(int ret){
    if (ret < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        else{
            cerr << strerror(errno) << endl;
            shutdown();
            exit(ret);
        }    
    }
}


//Function to add data cnter to the black list
void DataAcquisition::AddToGreyList(string key, Subscriber &sub) {
    grey_list[key] = (grey_list.find(key) != grey_list.end()) ? grey_list[key]+1 : 1;

    if(grey_list[key] >= 3){
        black_list[key] = sub;
        cout << "USER BLACKLISTED: " << sub.username << " " << key  << endl;
        subscribers.erase(key);

    }
}


// Authenticate function checks whether the client is allowed to subscribe or cancel a subscription
void DataAcquisition::authenticate(char cl_msg[BUF_LEN], struct sockaddr_in *cl_addr, int sockfd) {
    const int COMMAND = 0;
    const int USERNAME = 1;
    const int PASSWORD = 2;
 
    const char password[] = "Leaf";
    const char action_sub[] = "Subscribe";
    const char action_cnl[] = "Cancel";
    const char reply[] = "Subscribed";
    // Extract IP address and port from the client_addr struct 
    char IP_addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(cl_addr->sin_addr), IP_addr, INET_ADDRSTRLEN);
    int port = ntohs(cl_addr->sin_port);
    if(port == 0 || strcmp(IP_addr, "0.0.0.0") == 0) return;
    // Create a key based on the IP address and port
    string key = string(IP_addr) + ":" + to_string(port);
    // Extract command, username, and password from the client_msg
    const int MAX_MSG = 3;
    // idx: 0 - command, 1 - username, 2 - password,  "Subscribe",<username>,"Leaf" or "Cancel",<username>
    char extractedMsgs[MAX_MSG][BUF2_LEN]; 
    int idx = 0;
    char * token = strtok(cl_msg, ",");
    while(idx < MAX_MSG ) {
        memset(&extractedMsgs[idx], 0, BUF2_LEN);
        if(token == NULL) memcpy(&extractedMsgs[idx], "", BUF2_LEN);
        else memcpy(&extractedMsgs[idx], token, BUF2_LEN);
        token = strtok(NULL, ",");
        idx++;
    }
    // initalizing Subcriber Object 
    Subscriber sub;
    memcpy(&sub.username, &extractedMsgs[USERNAME], BUF2_LEN);
    memcpy(&sub.IP_addr, &IP_addr, INET_ADDRSTRLEN);
    sub.port = port;
    // If the client wants to subscribe and provides the correct password, add them to the subscribers list
    if( (strcmp(extractedMsgs[COMMAND], action_sub)==0) && (strcmp(extractedMsgs[PASSWORD], password) == 0)) {

        if (grey_list.find(key) == grey_list.end()) {

            pthread_mutex_lock(&lock_x);
            subscribers[key] = sub;
            pthread_mutex_unlock(&lock_x);

            cout << "username: " << extractedMsgs[USERNAME] <<" has SUBSCRIBED!!" << endl;

            sendto(sockfd, reply ,sizeof(reply), 0,(struct sockaddr *)cl_addr, sizeof(*cl_addr));

        } 
        else cout << extractedMsgs[USERNAME] <<" has ALREADY SUBSCRIBED." << endl;
        
    }
    // Subscriber wants to cancel subscription
    if(strcmp(extractedMsgs[COMMAND], action_cnl) == 0) {
        pthread_mutex_lock(&lock_x);
        subscribers.erase(key);
        pthread_mutex_unlock(&lock_x);
        cout << "username: " << sub.username << key << " has UNSUBSCRIBED!!" << endl;
    }
    // Handle rogue Data Centers attempting to guess password
    if((strcmp(extractedMsgs[COMMAND], action_sub) != 0) && strcmp(extractedMsgs[COMMAND], action_cnl) != 0) {
        cout << "DataAcquisition: UNKNOWN COMMAND " << extractedMsgs[COMMAND] << endl;
        AddToGreyList(key, sub);
    } else if(((strcmp(extractedMsgs[PASSWORD], password) != 0) && (strcmp(extractedMsgs[COMMAND], action_cnl) !=0))) 
      cout << "DataAcquisition: INVALID PASSWORD " << extractedMsgs[PASSWORD] << endl;
        AddToGreyList(key, sub);       
}


// Function to exit gracefully on errors
void DataAcquisition::gracefulExit(const string& errorMessage) {
    cerr << errorMessage << endl;
    shutdown();
    exit(EXIT_FAILURE);
}


/*run is the main function that initializes and starts the Data Acquisition program. 
It sets up shared memory, sockets, and threads, then starts reading the shared memory
*/
void DataAcquisition::run() {
    // Remove setupSignalHandler(); as it's moved to the constructor
    setupSharedMemory();
    setupSocket();
    createThreads();
    readMemory(); //NOTE: readMemory is blocking with a while loop

    int ret = pthread_join(rd_tid, NULL);
    if (ret != 0) {
        gracefulExit("Error: Failed to join receive thread.");
    }

    ret = pthread_join(wr_tid, NULL);
    if (ret != 0) {
        gracefulExit("Error: Failed to join send thread.");
    }

    shutdown();

    cout << "***QUITTING DATA ACQUISTION UNIT***" << endl; 
}


/*shutdown is a function that performs necessary cleanup operations when the program is terminating. 
It sets is_running to false, closes the socket, releases semaphore resources, detaches shared memory, removes the shared memory segment.
*/
void DataAcquisition::shutdown() {
    is_running = false;
    close(sockfd);
    sem_close(sem_id1);
    sem_unlink(SEMNAME);
    shmdt((void *)ShmPTR);
    shmctl(ShmID, IPC_RMID, NULL);
}
