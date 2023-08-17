#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <vector>
#include <string>
#include <cstring>
#include <map>
#include <list>

using namespace std;

void handleError(string msg)
{
    cerr << msg << " error code " << errno << " (" << strerror(errno) << ")\n";
    exit(1);
}

int main(int argc, char* argv[]) {
    int port = 28563, concurrentClientCount = 0;
    struct sockaddr_in serverAddress, clientAddress;
    int listenSocket;  // впускающий сокет
    vector<pollfd> allSockets; //хранилище всех сокетов (впускающий + клиентские) для функции poll
    vector<int> modeSockets; //users mods (0 - master socket, 1 - init, 2 - attempt to logg, 3 - create account, 4 - in portal)
    vector<string> online_logins;
    map<int, string> dataForProcessing; //неполные запросы от клиентов (продолжение которых еще не доставилось по сети)
    map<string, string> login;
    map<string, vector<string>> recovery_messages;
    char temp_answer_4[] = "-> No valid - try again <-\n";
    char temp_answer_3[] = "-> 1 - if u registred, 0 - if u want to be registred <-\n";
    char  temp_answer_2[] = "-> Are you registered or want to create an account? (1|0) <-\n";
    char  temp_answer[] = "-> give your login and password separated by a space <-\n";
    //создаем НЕБЛОКИРУЮЩИЙ сокет для приема соединений всех клиентов
    listenSocket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);

    //добавляем его в общий массив для функции poll
    pollfd tmpPollfd;
    tmpPollfd.fd = listenSocket;
    tmpPollfd.events = POLLIN;
    allSockets.push_back(tmpPollfd);
    modeSockets.push_back(0);
    online_logins.push_back("");

    //разрешаем повторно использовать тот же порт серверу после перезапуска (нужно, если сервер упал во время открытого соединения)
    int turnOn = 1;
    if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &turnOn, sizeof(turnOn)) == -1)
        handleError("setsockopt failed:");

    // Setup the TCP listening socket
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = inet_addr("0.0.0.0");
    serverAddress.sin_port = htons(port);

    if (bind( listenSocket, (sockaddr *) &serverAddress, sizeof(serverAddress)) == -1)
        handleError("bind failed:");

    if (listen(listenSocket, 1000) == -1) handleError("listen failed:");

    char *recvBuffer[1000];
    while (true)
    {
        list<int> disconnectedClients;
        //ждем подключения нового клиента или прихода запроса от какого-нибудь из уже подключенных клиентов
        if ( poll(&allSockets[0], allSockets.size(), -1) < 0 ) handleError("poll failed:");

        //выясняем на какой сокет пришли данные
        for (int i = 0; i < allSockets.size(); ++i)
        {
            if (allSockets[i].revents == 0) continue;
            if (allSockets[i].revents != POLLIN)
            {
                disconnectedClients.push_back(allSockets[i].fd);
                //cerr << "Error: wrong event type occured, when we waiting only for POLLIN\n";
                //exit(1);
            }
            if (i == 0)  //произошло подключение нового клиента
            {
                int clientSocket = accept(listenSocket, 0, 0);
                if (clientSocket < 0)
                {
                    if (errno != EWOULDBLOCK) handleError("accept failed: ");
                    else //в специфических случаях бывает, что accept не принимает клиента после срабатывания poll
                        continue;
                }
                //добавляем нового клиента
                tmpPollfd.fd = clientSocket;
                allSockets.push_back(tmpPollfd);
                modeSockets.push_back(1);
                cout << ++concurrentClientCount << " concurrent clients are connected\n";
                send( clientSocket,  &temp_answer_2, strlen(temp_answer_2), MSG_NOSIGNAL);
            }
            else //произошел прием данных от клиента
            {
                int clientSocket = allSockets[i].fd;
                int* clientmode = &modeSockets[i];
                int readBytesCount = 1;
                int err = 0;
                err = recv(clientSocket, recvBuffer, 10000, MSG_NOSIGNAL);
                if (err < 0)
                {
                    if (errno != EWOULDBLOCK) handleError("recv failed:");
                    else //данные не получены (такое бывает, но очень редко)
                        continue;
                }
                if (err == 0) //клиент разорвал соединение
                {
                    disconnectedClients.push_back(i);
                    continue;
                }
                //обрабатываем все поступившие от клиента запросы

                string data;
                data = "";
                data.append(reinterpret_cast<char*>(recvBuffer));
                string queries = dataForProcessing[clientSocket] + data;
                queries=queries.erase(queries.find_first_of("\r\n")+2);

                if (queries.find("exit\r\n") != std::string::npos)
                {
                    shutdown(clientSocket, SHUT_RDWR);
                    close(clientSocket);
                    disconnectedClients.push_back(i);
                    continue;
                }
                if (*clientmode == 1)
                    if (queries[0] == '1') {
                        *clientmode = 2;
                        send( clientSocket,  &temp_answer, strlen(temp_answer), MSG_NOSIGNAL);
                        continue;
                    }
                    else if (queries[0] == '0') {
                        *clientmode = 3;
                        send( clientSocket,  &temp_answer, strlen(temp_answer), MSG_NOSIGNAL);
                        continue;
                    }
                    else {
                        send(clientSocket, temp_answer_3, strlen(temp_answer_3), MSG_NOSIGNAL);
                        continue;
                    }
                else if (*clientmode == 2 || *clientmode == 3)
                {
                    if (std::count(queries.begin(), queries.end(), ' ') != 1 || queries.find(' ')+4 == string::npos || queries.find(' ') == 0)
                    {
                        send(clientSocket, temp_answer_4, strlen(temp_answer_4), MSG_NOSIGNAL);
                        continue;
                    }
                    int border1 = 0, border2 = queries.find(" "),border3 = queries.find("\r\n");

                    string log = queries.substr(border1, border2 - border1);
                    string passw = queries.substr(border2+1, border3 - border2-1);

                    map<string, string>::iterator it;

                    if (*clientmode == 2)
                    {
                        if ((it = login.find(log)) != login.end())
                            if (it->second == passw) {
                                online_logins.push_back(log);
                                send(clientSocket, "-> you in chat <-\n", 18, MSG_NOSIGNAL);
                                send(clientSocket, "-> Yours missed messages: <-\n", 29, MSG_NOSIGNAL);
                                // go out all messages;
                                if (auto item = recovery_messages.find(log); item != recovery_messages.end())
                                {
                                    for (int j = 0; j < item->second.size(); j++) {
                                        char temp_message[item->second[j].length() + 1];
                                        strcpy(temp_message, item->second[j].c_str());
                                        send(clientSocket,temp_message, item->second[j].length()+1, MSG_NOSIGNAL);
                                    }
                                    recovery_messages.erase(recovery_messages.find(log));
                                }
                                //
                                send(clientSocket, "-> if u want to create some message do exp. 'login message' "
                                                   "exit chat 'exit' <-\n", 80, MSG_NOSIGNAL);
                                *clientmode = 4;
                                continue;
                            }
                        send(clientSocket, temp_answer_4, strlen(temp_answer_4), MSG_NOSIGNAL);
                        continue;
                    }
                    if (*clientmode == 3) {
                        if ((it = login.find(log)) == login.end()) {
                            send(clientSocket, "-> success <-\n", 14, MSG_NOSIGNAL);
                            login.insert(std::map<string, string>::value_type(log, passw));
                            send( clientSocket,  &temp_answer, strlen(temp_answer), MSG_NOSIGNAL);
                            *clientmode = 2;
                            continue;
                        }
                        send(clientSocket, temp_answer_4, strlen(temp_answer_4), MSG_NOSIGNAL);
                        continue;
                    }
                }
                else // 4 mode in chat
                {
                    if (std::count(queries.begin(), queries.end(), ' ') != 1 || (queries.rfind(' ')+4) == string::npos || queries.find(' ') == 0)
                    {
                        send(clientSocket, temp_answer_4, strlen(temp_answer_4), MSG_NOSIGNAL);
                        continue;
                    }

                    int border1 = 0, border2 = queries.find(" "),border3 = queries.find("\r\n");
                    string log = queries.substr(border1, border2 - border1);
                    string message = queries.substr(border2+1, border3 - border2-1);
                    string answer = online_logins[i] + " -> " + message;
                    answer.append(1,'\n');
                    char temp_message[answer.length()+1];
                    strcpy(temp_message, answer.c_str());

                    if (auto key = (find(begin(online_logins), end(online_logins), log)); key != online_logins.end())
                        send(allSockets[std::distance(online_logins.begin(), key)].fd,temp_message, answer.length()+1, MSG_NOSIGNAL);
                    else
                    {
                        if (recovery_messages.find(log) != recovery_messages.end())
                            recovery_messages[log].push_back(temp_message);
                        else
                        {
                            vector<string> vec(1);
                            vec.push_back(temp_message);
                            recovery_messages.insert(std::map<string,vector<string>>::value_type(log,vec));
                        }
                    }
                }
            }
        }
        // удаляем отключившихся клиентов из всех структур
        for (list<int>::reverse_iterator it = disconnectedClients.rbegin(); it != disconnectedClients.rend(); ++it)
        {
            dataForProcessing.erase(allSockets[*it].fd);
            allSockets.erase(*it+allSockets.begin());
            modeSockets.erase(*it + modeSockets.begin());
            online_logins.erase(*it + online_logins.begin());
            concurrentClientCount--;
        }
    }
}