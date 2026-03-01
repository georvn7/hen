// hen.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "Server.h"
#include "Client.h"
#include "Utils.h"

using namespace hen;

int main(int argc, char* argv[])
{
    std::cout << std::endl << std::endl;
    std::cout << Peer::getHeader();
    std::cout << std::endl;

    bool server = false;
    bool client = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "-server")
        {
            server = true;
        }
        else if (std::string(argv[i]) == "-client")
        {
            client = true;
        }
    }

    if (server)
    {
        Server::getInstance().init(argc, argv);
    }
    if (client)
    {
        Client::getInstance().init(argc, argv);
    }

    //When we run client, the client will decide when to stop the app main loop
    while (server || client)
    {
        if (server)
        {
            server = Server::getInstance().update();
        }
        if (client)
        {
            client = Client::getInstance().update();
        }
    }
    
    if (client)
    {
        Client::getInstance().shutdown();
    }
    if (server)
    {
        Server::getInstance().shutdown();
    }
}
