#include <stdio.h>      // standard IO
#include <stdlib.h>     // general utilities
#include <string.h>     // string operations
#include <unistd.h>     // close, read
#include <fcntl.h>      // fcntl
#include <errno.h>      // error handling
#include <netdb.h>      // getaddrinfo
#include <sys/socket.h> // socket APIs
#include <sys/epoll.h>  // epoll APIs
#include <arpa/inet.h>  // internet structures
#include <openssl/sha.h> // SHA1 hashing
#include <openssl/evp.h> // Base64 encoding
#include <sys/select.h>  // FD_SETSIZE

#define PORT "8080"        // server listening port
#define MAX_EVENTS 128     // max epoll events per wait
#define BUFFER_SIZE 8192   // socket read buffer size
#define MAX_GAMES 100      // max simultaneous games
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" // websocket magic guid


/*
Game structure stores one active match.
Each game contains:
- player1 socket
- player2 socket
- 3x3 board (9 cells)
- current turn
- active flag
*/
typedef struct {
    int player1;     
    int player2;     
    char board[9];   
    char turn;       
    int active;      
} Game;


/* Global game storage */
Game games[MAX_GAMES];   

/* 
If only one player is connected,
he waits here until second player joins
*/
int waiting_player = -1; 


/*
Track whether a client has upgraded to WebSocket.
0 = HTTP connection
1 = WebSocket connection
This prevents checking "Upgrade: websocket"
multiple times for same client.
*/
int is_websocket[FD_SETSIZE] = {0};


/*
Enable non-blocking mode on socket.
Required because epoll works best
with non-blocking file descriptors.
*/
int set_nonblocking(int fd) {

    // fcntl(fd, F_GETFL, 0) -> get flags
    int flags = fcntl(fd, F_GETFL, 0);

    // fcntl(fd, F_SETFL, flags | O_NONBLOCK)
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


/*
Return MIME type based on file extension.
Used while serving static HTTP files.
*/
const char* get_mime_type(const char *path) {

    // strstr checks if substring exists
    if(strstr(path, ".js")) return "application/javascript";
    if(strstr(path, ".css")) return "text/css";
    if(strstr(path, ".html")) return "text/html";
    return "text/plain";
}


/*
Serve static file for HTTP GET request.
Sends HTTP header followed by file content.
*/
void serve_file(int client_fd, const char *path) {

    char filepath[256];

    /* If root path "/", serve index.html */
    if(strcmp(path, "/") == 0)
        strcpy(filepath, "index.html");
    else
        // snprintf safe formatted copy
        snprintf(filepath, sizeof(filepath), "%s", path + 1);

    /* Open requested file */
    int fd = open(filepath, O_RDONLY);

    if(fd < 0) {

        /* If file not found, return 404 */
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n\r\n";

        // send(socket, buffer, length, flags)
        send(client_fd, not_found, strlen(not_found), 0);
        return;
    }

    /* Send HTTP 200 header */
    char header[256];

    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n\r\n",
        get_mime_type(filepath));

    send(client_fd, header, strlen(header), 0);

    /* Send file content */
    char buffer[BUFFER_SIZE];
    int bytes;

    // read(fd, buffer, size)
    while((bytes = read(fd, buffer, BUFFER_SIZE)) > 0)
        send(client_fd, buffer, bytes, 0);

    // close(fd) release descriptor
    close(fd);
}


/* Initialize board with empty spaces */
void init_board(char *b){
    for(int i=0;i<9;i++)
        b[i]=' ';
}


/* Check win combinations */
int check_winner(char *b){

    int win[8][3]={{0,1,2},{3,4,5},{6,7,8},
                   {0,3,6},{1,4,7},{2,5,8},
                   {0,4,8},{2,4,6}};

    for(int i=0;i<8;i++)
        if(b[win[i][0]]!=' ' &&
           b[win[i][0]]==b[win[i][1]] &&
           b[win[i][1]]==b[win[i][2]])
            return 1;

    return 0;
}


/* Check draw condition */
int check_draw(char *b){
    for(int i=0;i<9;i++)
        if(b[i]==' ') return 0;
    return 1;
}


/* Find game that contains this client */
Game* find_game(int client){
    for(int i=0;i<MAX_GAMES;i++)
        if(games[i].active &&
          (games[i].player1==client ||
           games[i].player2==client))
            return &games[i];
    return NULL;
}


/* Mark game inactive */
void cleanup_game(Game *g){
    g->active=0;
}

/*
Send a small WebSocket text frame.
Frame format:
Byte 1 -> FIN + opcode
Byte 2 -> payload length
Then payload
*/
int ws_send(int fd,const char *msg){

    size_t len=strlen(msg);

    unsigned char frame[256];

    frame[0]=0x81;  // FIN + text frame
    frame[1]=len;   // payload length

    memcpy(frame+2,msg,len);

    return send(fd,frame,len+2,0);
}


/*
Start new game between two players.
Assign symbols and notify both players.
*/
void start_game(int p1,int p2){

    for(int i=0;i<MAX_GAMES;i++){

        if(!games[i].active){

            games[i].player1=p1;
            games[i].player2=p2;
            games[i].turn='X';
            games[i].active=1;

            init_board(games[i].board);

            ws_send(p1,"{\"type\":\"start\",\"symbol\":\"X\"}");
            ws_send(p2,"{\"type\":\"start\",\"symbol\":\"O\"}");
            return;
        }
    }
}


/*
Validate and apply move.
Broadcast update and check result.
*/
void handle_move(int client_fd,int position){

    Game *g=find_game(client_fd);
    if(!g) return;

    char symbol=(g->player1==client_fd)?'X':'O';

    /* Invalid move check:
       - Not your turn
       - Position already filled
    */
    if(g->turn!=symbol || g->board[position]!=' ')
        return;

    /* Apply move */
    g->board[position]=symbol;

    /* Switch turn */
    g->turn=(symbol=='X')?'O':'X';

    char update[128];

    snprintf(update,sizeof(update),
    "{\"type\":\"update\",\"position\":%d,\"symbol\":\"%c\"}",
    position,symbol);

    /* Send board update to both players */
    ws_send(g->player1,update);
    ws_send(g->player2,update);

    /* Check win */
    if(check_winner(g->board)){

        char winmsg[64];

        snprintf(winmsg,sizeof(winmsg),
        "{\"type\":\"win\",\"winner\":\"%c\"}",symbol);

        ws_send(g->player1,winmsg);
        ws_send(g->player2,winmsg);

        cleanup_game(g);
    }
    /* Check draw */
    else if(check_draw(g->board)){

        ws_send(g->player1,"{\"type\":\"draw\"}");
        ws_send(g->player2,"{\"type\":\"draw\"}");

        cleanup_game(g);
    }
}
/*
Handling forced exit request.
Opponent is declared winner.
*/
void handle_exit(int client_fd){

    Game *g = find_game(client_fd);
    if(!g) return;

    int opponent =
        (g->player1 == client_fd) ?
        g->player2 : g->player1;

    if(opponent > 0){

        char winner =
            (g->player1 == client_fd) ? 'O' : 'X';

        char winmsg[64];

        snprintf(winmsg,sizeof(winmsg),
        "{\"type\":\"win\",\"winner\":\"%c\"}",
        winner);

        ws_send(opponent, winmsg);
    }

    cleanup_game(g);
}


/*
Decode WebSocket frame.
Client frames are masked.
We unmask payload using masking key.
*/
int ws_decode_frame(char *buffer, int bytes, char *data_out) {

    if(bytes < 6) return -1;

    int payload_len = buffer[1] & 127;

    unsigned char *mask = (unsigned char*)(buffer + 2);

    for(int i=0;i<payload_len;i++)
        data_out[i] = buffer[i+6] ^ mask[i%4];

    data_out[payload_len] = '\0';

    return payload_len;
}


/*
Handle WebSocket message types.
Currently supports:
- move
- exit
*/
void ws_handle_message(int client_fd, char *data) {

    if(strstr(data, "\"type\":\"move\"")) {

        int pos;
        if(sscanf(data,
        "{\"type\":\"move\",\"position\":%d}",
        &pos)==1)
            handle_move(client_fd,pos);
    }
    else if(strstr(data, "\"type\":\"exit\"")) {

        handle_exit(client_fd);
        close(client_fd);
    }
}


/*
Perform WebSocket handshake.
Steps:
1. Extract Sec-WebSocket-Key
2. Append GUID
3. SHA1 hash
4. Base64 encode
5. Send HTTP 101 response
*/
int websocket_handshake(int client_fd, char *request){

    /*
    Find "Sec-WebSocket-Key" header inside HTTP request.
    */
    char *key=strstr(request,"Sec-WebSocket-Key:");
    if(!key) return -1;

    /*
    Move pointer after header name.
    "Sec-WebSocket-Key:" is 19 characters long.
    */
    key+=19;

    while(*key==' ')
        key++;


    /*
    ws_key will store only the key value extracted
    from HTTP header.
    */
    char ws_key[128]={0};

    int i=0;

    /*
    Copy characters
    HTTP headers end with \r\n.
    */
    while(key[i]!='\r'&&key[i]!='\n')
        ws_key[i++]=key[i];


    /*
    combined = client_key + GUID
    */
    char combined[256];

    snprintf(combined,sizeof(combined),
             "%s%s",ws_key,WS_GUID);


    /*
    SHA1 digest produces 20 byte hash.
    SHA_DIGEST_LENGTH = 20
    */
    unsigned char sha1[SHA_DIGEST_LENGTH];

    /*
    Compute SHA1 hash of combined string.
    Input  -> combined string
    Length -> strlen(combined)
    Output -> sha1 buffer
    */
    SHA1((unsigned char*)combined,
         strlen(combined),sha1);


    /*
    Base64 encoded version of SHA1 hash.
    Browser expects this encoded value
    in Sec-WebSocket-Accept header.
    */
    char base64[256];

    /*
    EVP_EncodeBlock converts raw binary
    sha1 hash into base64 string.
    */
    EVP_EncodeBlock((unsigned char*)base64,
                    sha1,SHA_DIGEST_LENGTH);


    /*
    Prepare HTTP 101 Switching Protocols response.
    This tells browser to switch from HTTP to WebSocket.
    */
    char response[512];

    int len=snprintf(response,sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        base64);


    /*
    Send handshake response to client.
    After this, connection becomes WebSocket.
    */
    return send(client_fd,response,len,0);
}

/*
Handle WebSocket upgrade request.
Called only once per client.
*/
void ws_handle_connection(int client_fd, char *buffer) {

    websocket_handshake(client_fd,buffer);

    /* Mark this client as WebSocket */
    is_websocket[client_fd] = 1;

    /* Pair players */
    if(waiting_player==-1)
        waiting_player=client_fd;
    else{
        start_game(waiting_player,client_fd);
        waiting_player=-1;
    }
}

/*
Main event loop.
Handles:
- New connections
- HTTP requests
- WebSocket messages
*/
int main(){

    struct addrinfo hints,*res;
    int server_fd;

    // memset initialize struct
    memset(&hints,0,sizeof(hints));

    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_flags=AI_PASSIVE;

    // getaddrinfo resolve address
    getaddrinfo(NULL,PORT,&hints,&res);

    // socket create endpoint
    server_fd=socket(res->ai_family,
                     res->ai_socktype,
                     res->ai_protocol);

    int yes=1;

    // setsockopt reuse address
    setsockopt(server_fd,SOL_SOCKET,
               SO_REUSEADDR,&yes,sizeof(yes));

    // bind attach socket to port
    bind(server_fd,res->ai_addr,res->ai_addrlen);

    // listen start listening
    listen(server_fd,20);

    set_nonblocking(server_fd);

    // epoll_create1 create epoll instance
    int epoll_fd=epoll_create1(0);

    struct epoll_event ev,events[MAX_EVENTS];

    ev.events=EPOLLIN;
    ev.data.fd=server_fd;

    // epoll_ctl add server socket
    epoll_ctl(epoll_fd,EPOLL_CTL_ADD,
              server_fd,&ev);

    printf("Server running on http://localhost:8080\n");
    
    while(1){

        // epoll_wait(epollfd, events, maxevents, timeout)
        int n=epoll_wait(epoll_fd,
                         events,MAX_EVENTS,-1);

        for(int i=0;i<n;i++){

            /* New connection */
            if(events[i].data.fd==server_fd){

                // accept(listen_socket, addr, addrlen)
                int client_fd=
                    accept(server_fd,NULL,NULL);

                set_nonblocking(client_fd);

                ev.events=EPOLLIN;
                ev.data.fd=client_fd;

                epoll_ctl(epoll_fd,
                          EPOLL_CTL_ADD,
                          client_fd,&ev);
            }
            else{

                int client_fd=
                    events[i].data.fd;

                char buffer[BUFFER_SIZE];

                // recv(socket, buffer, size, flags)
                int bytes=
                    recv(client_fd,
                         buffer,BUFFER_SIZE,0);

                /* Client disconnected */
                if(bytes<=0){

                    handle_exit(client_fd);

                    if(waiting_player == client_fd)
                        waiting_player = -1;

                    is_websocket[client_fd] = 0;

                    close(client_fd);
                    continue;
                }
                
                buffer[bytes]='\0';

                /* If still HTTP */
                if(!is_websocket[client_fd]) {

                    /* WebSocket upgrade request */
                    if(strstr(buffer,"Upgrade: websocket")) {
                        ws_handle_connection(client_fd, buffer);
                    }
                    /* Normal HTTP GET */
                    else if(strncmp(buffer,"GET",3)==0){

                        char method[8],path[256];

                        sscanf(buffer,"%s %s",
                               method,path);

                        serve_file(client_fd,path);
                        close(client_fd);
                    }
                }
                else{
                    /* Already WebSocket connection */
                    char data[BUFFER_SIZE];

                    int len = ws_decode_frame(buffer, bytes, data);

                    if(len > 0)
                        ws_handle_message(client_fd, data);
                }
            }
        }
    }

    return 0;
}