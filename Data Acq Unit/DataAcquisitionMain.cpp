#include "DataAcquisition.h"

int main(int argc, char* argv[]) {
    // Create an instance of the DataAcquisition class
    DataAcquisition dataAcquisition;

    // Initialize and run the data acquisition unit
    dataAcquisition.initialize();
    dataAcquisition.run();

    return 0;
}


