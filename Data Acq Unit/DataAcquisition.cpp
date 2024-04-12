#include "DataAcquisition.h"

DataAcquisition* DataAcquisition::instance = nullptr;

static void signalHandler(int sigint) {
    switch(sigint) {
        case SIGINT:
            cout << "DataAcquisition shutting..." << endl;
            DataAcquisition::instance->shutdown();
            break;
    }
}

// Function to exit gracefully on errors
void DataAcquisition::gracefulExit(const string& errorMessage) {
    cerr << errorMessage << endl;
    shutdown();
    exit(EXIT_FAILURE);
}


DataAcquisition::DataAcquisition() {
    // lock_x = PTHREAD_MUTEX_INITIALIZER;
    is_running=false;
    ShmPTR=nullptr;
    DataAcquisition::instance = this;
}


void DataAcquisition::setupSharedMemory() {
    // TODO: Implement your shared memory setup here
    // Create a shared memory key
    ShmKey = ftok("MEMNAME", 65);
    if (ShmKey == -1) {
        cerr << "Error creating shared memory key: " << strerror(errno) << endl;
        exit(1);
    }

    // Create a shared memory segment
    ShmID = shmget(ShmKey, sizeof(SeismicMemory), IPC_CREAT | 0666);
    if (ShmID == -1) {
        cerr << "Error creating shared memory segment: " << strerror(errno) << endl;
        exit(1);
    }

    // Attach the shared memory segment to the process
    ShmPTR = (struct SeismicMemory *)shmat(ShmID, NULL, 0);
    if ((intptr_t)ShmPTR == -1) {
        cerr << "Error attaching shared memory segment: " << strerror(errno) << endl;
        exit(1);
    }

    // Create a named semaphore
    sem_id1 = sem_open(SEMNAME, O_CREAT, SEM_PERMS, 0);
    if (sem_id1 == SEM_FAILED) {
        cerr << "Error creating semaphore: " << strerror(errno) << endl;
        exit(1);
    }
    is_running = true;

}

void DataAcquisition::setupSignalHandler() {
    struct sigaction action;
    action.sa_handler = signalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    signal(SIGINT, signalHandler);
}

void DataAcquisition::setupSocket() {
    // TODO: Implement your socket setup here
    // Create a UDP socket
    sv_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sv_sock < 0) {
        cerr << "Error creating socket: " << strerror(errno) << endl;
        exit(1);
    }

    // Configure the server address
    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddress.sin_port = htons(1153);

    // Bind the socket to the server address
    if (bind(sv_sock, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0) {
        cerr << "Error binding socket: " << strerror(errno) << endl;
        close(sv_sock);
        exit(1);
    }
    
}

void DataAcquisition::createThreads() {
    pthread_mutex_init(&lock_x, NULL);
    int ret;

    ret = pthread_create(&rd_tid, NULL, recv_func, this);
    if (ret != 0) {
        gracefulExit("Error: Failed to create receive thread.");
    }

    ret = pthread_create(&wr_tid, NULL, send_func, this);
    if (ret != 0) {
        gracefulExit("Error: Failed to create send thread.");
    }
}

void DataAcquisition::authenticate(char cl_msg[BUF_LEN], struct sockaddr_in *cl_addr, int sv_sock) {
    string ipAddress(inet_ntoa(cl_addr->sin_addr));

    // Check if the IP address belongs to a rogue data center
    if (black_list.find(ipAddress) != black_list.end()) {
        cerr << "Rogue data center detected at " << ipAddress << ":" << ntohs(cl_addr->sin_port) << endl;
        return;
    }

    string message(cl_msg);
    istringstream iss(message);
    string command, username, password;

    getline(iss, command, ',');
    getline(iss, username, ',');
    getline(iss, password, ',');

    // Trim the quotes from the username
    username = username.substr(1, username.size() - 2);

    if (command == "Subscribe" && password == "Leaf") {
        // Create a new subscriber
        Subscriber sub;
        strncpy(sub.username, username.c_str(), sizeof(sub.username));
        strncpy(sub.IP_addr, ipAddress.c_str(), sizeof(sub.IP_addr));
        sub.port = ntohs(cl_addr->sin_port);

        // Add the subscriber to the list
        subscribers[username] = sub;

        // Send a confirmation message to the subscriber
        string confirmationMsg = "Subscribed";
        int sentSize = sendto(sv_sock, confirmationMsg.c_str(), confirmationMsg.size(), 0, (struct sockaddr *)cl_addr, sizeof(*cl_addr));
        if (sentSize < 0) {
            cerr << "Error sending confirmation message to the subscriber: " << strerror(errno) << endl;
        } else {
            cout << "Subscriber " << username << " has subscribed." << endl;
        }

        // Clear the IP address from the grey_list
        grey_list.erase(ipAddress);
    } else if (command == "Cancel" && subscribers.find(username) != subscribers.end()) {
        // Remove the subscriber from the list
        subscribers.erase(username);
        cout << "Subscriber " << username << " has unsubscribed." << endl;
    } else {
        cerr << "Failed authentication attempt from " << ipAddress << ":" << ntohs(cl_addr->sin_port) << endl;

        // Add the IP address to the grey_list and count the failed attempts
        grey_list[ipAddress]++;

        // If the number of failed attempts exceeds the threshold, add the IP address to the black_list
        if (grey_list[ipAddress] >= MAX_FAILED_ATTEMPTS) {
            Subscriber rogue;
            strncpy(rogue.IP_addr, ipAddress.c_str(), sizeof(rogue.IP_addr));
            rogue.port = ntohs(cl_addr->sin_port);
            black_list[ipAddress] = rogue;

            cerr << "Rogue data center detected at " << ipAddress << ". Added to the black list." << endl;
        }
    }
}


void DataAcquisition::check(int value) {
    // TODO: Implement error checking logic here
    if (value < 0) {
        cerr << "Error: " << strerror(errno) << endl;
        exit(EXIT_FAILURE);
    }
}

const int SUSPICIOUS_THRESHOLD = 3; // Adjust this value based on your needs

void DataAcquisition::AddToGreyList(string key, Subscriber &sub) {
    // TODO: Implement grey list addition logic here
    // If subscriber is already in the grey list
    if (grey_list.find(key) != grey_list.end()) {
        grey_list[key]++;

        // If the subscriber's count exceeds the threshold, move them to the blacklist
        if (grey_list[key] >= SUSPICIOUS_THRESHOLD) {
            black_list[key] = sub;
            grey_list.erase(key);
            cout << "Subscriber " << sub.username << " (" << sub.IP_addr << ":" << sub.port << ") has been blacklisted." << endl;
        }
    } else {
        // Add the subscriber to the grey list with a count of 1
        grey_list[key] = 1;
    }
}

void DataAcquisition::readMemory() {
    // TODO: Implement reading from shared memory here
    while (is_running) {
        // Wait for the semaphore to ensure data is available
        sem_wait(sem_id1);

        // Check if the status of the packet in shared memory is set to WRITTEN
        if (ShmPTR->status == WRITTEN) {

            // Read the data from shared memory
            DataPacket packet;
            packet.packetNo = ShmPTR->packetNumber;
            packet.packetLen = ShmPTR->length;
            packet.data = string(ShmPTR->data, ShmPTR->length);

            // Push the data packet onto the packet queue
            pthread_mutex_lock(&lock_x);
            packetQueue.push(packet);
            pthread_mutex_unlock(&lock_x);

            // Set the status of the packet in shared memory to READ
            ShmPTR->status = READ;
        }

        // Sleep for 1 second
        sleep(1);
    }
}

void* DataAcquisition::recv_func(void *arg) {
    // TODO: Implement receiver function here
    DataAcquisition *da = static_cast<DataAcquisition*>(arg);
    char buffer[BUF_LEN];
    struct sockaddr_in clientAddress;
    socklen_t addressLength;

    while (da->is_running) {
        memset(buffer, 0, BUF_LEN);
        addressLength = sizeof(clientAddress);
        ssize_t receivedSize = recvfrom(da->sv_sock, buffer, BUF_LEN - 1, 0,
                                        (struct sockaddr *)&clientAddress, &addressLength);

        if (receivedSize > 0) {
            memset(IP_addr, 0, INET_ADDRSTRLEN);
            int ret = inet_ntop(AF_INET, &(cl_addr.sin_addr), IP_addr, INET_ADDRSTRLEN);
            if (ret = NULL){
                cerr << "Error: Failed to convert IP address to string format." << endl;
                }
                else{
                    port = ntohs(cl_addr.sin_port);
                    string key = string(IP_addr) + ":" + to_string(port);
                    // Process the received message and authenticate the data center
                    if (da->black_list.find(key) == da->black_list.end()) {
                        da->authenticate(buffer, &clientAddress, da->sv_sock);
                        }
                    }
        }
        memset(&buffer, 0, BUF_LEN);
    }

    cout << "[DEBUG] Receive thread exiting... " << endl; 
    pthread_exit(NULL);
}

void* DataAcquisition::send_func(void *arg) {
    // TODO: Implement sender function here
    DataAcquisition *da = static_cast<DataAcquisition*>(arg);

    while (da->is_running) {
        pthread_mutex_lock(&(da->lock_x));
        while (!da->packetQueue.empty()) {
            DataPacket packet = da->packetQueue.front();
            da->packetQueue.pop();

            for (const auto &entry : da->subscribers) {
                const Subscriber &subscriber = entry.second;
                struct sockaddr_in subscriberAddress;
                memset(&subscriberAddress, 0, sizeof(subscriberAddress));
                subscriberAddress.sin_family = AF_INET;
                inet_pton(AF_INET, subscriber.IP_addr, &subscriberAddress.sin_addr);
                subscriberAddress.sin_port = htons(subscriber.port);

                ssize_t sentSize = sendto(da->sv_sock, packet.data.c_str(), packet.packetLen, 0,
                                          (struct sockaddr *)&subscriberAddress, sizeof(subscriberAddress));

                if (sentSize < 0) {
                    cerr << "Error sending data to subscriber."<< strerror(errno) << endl;
                }
            }
        }
        pthread_mutex_unlock(&(da->lock_x));
        sleep(1); // Sleep for 1s
    }

    cout << "[DEBUG] Send thread exiting..." << endl;
    pthread_exit(NULL);
}

void DataAcquisition::run() {
    setupSignalHandler();
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

    cout << "[DEBUG] Data Acquisition Unit exiting... " << endl; 
}

void DataAcquisition::shutdown() {
    is_running = false;
   // TODO: CLOSE SOCKETS
    close(sv_sock);
    // // Join the read and write threads
    // pthread_join(rd_tid, NULL);
    // pthread_join(wr_tid, NULL);
    // TODO: CLOSE SEM
    sem_close(sem_id1);
    sem_unlink(SEMNAME);
    // TODO: CLOSE SHM
    shmdt((void *)ShmPTR);
    shmctl(ShmID, IPC_RMID, NULL);

}



// int main() {
//     DataAcquisition dataAcquisition;
//     DataAcquisition::instance = &dataAcquisition;
//     dataAcquisition.run();
//     return 0;
// }
