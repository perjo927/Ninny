////////////////////////////////////////
////           PROXY.CC             ////
///                                  ///
// Hannah Börjesson, Per Jonsson, IP1 //
// Linköping university, 2013-02 ///////
// hanbo174, perjo927 //////////////////
// Group A14 ///////////////////////////
////////////////////////////////////////

#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <map>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

/*
  When compiling for Solaris or SunOS, you need to specify some extra
  command-line switches  for linking in the proper libraries.
  In order to do this, simply add "-lnsl -lsocket -lresolv"
  to the end of the compile command
*/

#define BACKLOG 10 // How many pending connections
#define CLNT_PRT 80 // 80 = HTTP, client port
#define MAX_DATA_SIZE 2048 // Buffer

using namespace std;

/////////////////////////////////////////////
////////////////// FUNCTIONS ////////////////
/////////////////////////////////////////////

// Takes care of zombie processes
void sigchld_handler(int s)
{
  while(waitpid(-1, nullptr, WNOHANG) > 0);
}

// Hash http header
void hdr_to_map(map<string, string>& http_hdr, string const& text) 
{
  // parse the fields differently
  #define REQUEST 0
  #define REST 1
  int field = REQUEST; 
    
  // flag for when to fill in mey or value in map
  #define KEY 0
  #define VALUE 1
  int map_pos = KEY; 
  stringstream text_stream;
  text_stream << text;  // string text to stringstream 
  string map_key, map_val, row;  

  // one row at a time
  while (getline(text_stream, row))
  {   
     // make sure we have something to parse
    if (*row.begin() != '\r')
    {
      // special case
      if (field  == REQUEST) http_hdr["request: "] = row; 
      // treat the others equally
      else if (field == REST )
      {
        map_key.clear(); map_val.clear(); // reset for new row of parsing        
        map_pos = KEY; // fetch key-part first
        transform(row.begin(), row.end(), row.begin(), ::tolower); // turn to lowercase      

        // step character by character (c)
        string::iterator c; 
        for (c = row.begin(); c != row.end(); ++c) // annan loop? ****
        {
          // make text before colon become the key (first), the rest the value (second)
          if (map_pos == KEY)   map_key.push_back(*c);       
          else if (map_pos == VALUE)  map_val.push_back(*c); 
          if (*c == ' ')  map_pos = VALUE; // switch after the ":" plus following whitespace
        }
        // map each keyword with a value
        map_val.pop_back();
        http_hdr[map_key] = map_val; 
      }
    }  
    // if *row.begin() == '\r' do nothing 
    else break; 
    
    // switch fields
    field = REST;
  }

  cout << endl << "***BEFORE**** " << http_hdr["request: "] << endl ;

  // Erase host field from request field - if present
  if (http_hdr["request: "] != "") 
  {
    string host = "http://";
    host += http_hdr["host: "];
    size_t start_pos = http_hdr["request: "].find(host);
  
    if (start_pos != string::npos)  http_hdr["request: "].replace(start_pos, host.length(), ""); 
  }  
} 


// sometimes we need to create our own header for interaction
void create_http_hdr(string& new_hdr, map<string, string> http_hdr) 
{
  // Fill in the first ones manually
  new_hdr += (http_hdr["request: "] + "\r\n");
  new_hdr += ("host: " + http_hdr["host: "] + "\r\n");	
  new_hdr += "connection: close\r\n";

  // Fill in the rest automatically, skip the ones already added
  for (auto h : http_hdr)
  {
    if (!(h.first == "request: " || h.first == "host: "
          || h.first == "connection: " || h.first == "proxy-connection: "))
    {
      new_hdr += h.first + h.second + "\r\n";
    }
  }
  // add carriage return at last
  new_hdr += "\r\n"; 
}

// Used for redirecting
int forbidden_words(string content)
{
  // "transform" to lower case to to make it non-case sensitive
  transform(content.begin(), content.end(), content.begin(), ::tolower);

  // forbidden words
  vector<string> bad_content = {"britney spears", "norrköping", "paris hilton", "spongebob"};

  // string::npos  means that the search string could not be found
  for (auto w : bad_content)
  {
    if (content.find(w) != string::npos)
    {
      cout << endl << endl << "Forbidden word detected: " << w << endl;
      return 1;
    }
  }
  return 0;
} 


// Simultaneously check multiple sockets to see if they
// have data waiting to be recv()d, or if you can send()
// data to them without blocking (or an exception has occurred)
int check_socket(int sock, int time_out) 
{
  int num;
  fd_set fds;
  struct timeval tv;

  // file descriptor sets
  FD_ZERO(&fds);// clear the set ahead of time
  FD_SET(sock,&fds);// add our descriptors to the set ** old-style cast

  //  tell select() how long to check  these sets for. .
  tv.tv_sec = time_out; // timeout in seconds
  tv.tv_usec = 0; // microseconds added to timeout

  // get either num of bytes oran  error message
  num = select(sock + 1, &fds, nullptr, nullptr, &tv);
  // 0 = timeout, -1 = error
  if (num != 0 || num != -1 ) return num;  // success
  
  // fail
  return 0; 
} 


// interact with server
void conversation(string& resp_string, string const& msg, string const& adr, int cnnctr_sock)
{
  int clnt_sock;
  sockaddr_in clnt;
  
  // we want TCP/IP
  if ((clnt_sock = socket(AF_INET, SOCK_STREAM, 0))  == -1) cout << endl << "Client: Unable to create socket" << endl;
  cout << endl << "Created client socket, conversation initiated with: " << adr <<  endl;  

  // struct hostent has a number of fields that contain information about the host in question.
  // takes a string like “www.yahoo.com”, and returns a struct hostent which
  // contains tons of information, including the IP address.
  hostent *host_info;
  string err_msg = "No host found - check IP or try reloading the page (F5)";

  // Couldn't return struct, handle error 
  if ((host_info  = gethostbyname(adr.c_str())) == nullptr) 
  {    
    send(cnnctr_sock, err_msg.c_str(), err_msg.size(), 0);    
    // Tell browser about our unhappy misfortunes
    cout << endl << "Host (IP address): " << adr << " not found" << endl;    
  }
  // else
  cout << endl << "Got IP number for host from gethostbyname()" << endl;

  // prepare to connect client
  clnt.sin_family = AF_INET;      
  clnt.sin_addr.s_addr = reinterpret_cast<in_addr*>(host_info->h_addr)->s_addr;       
  clnt.sin_port = htons(CLNT_PRT); // byte conversion                          

  // make sure the struct is empty before proceeding
  cout << endl << "Client struct: setting to empty" << endl;
  memset(clnt.sin_zero, 0, sizeof(clnt.sin_zero));
  
  // if connect() returns 0 all is well and everything is unfolding as it should
  // if connect() returns -1 something went wrong
  int connected = connect(clnt_sock, (struct sockaddr*)&clnt, sizeof(clnt));  
  if (connected == -1) cerr << "Client: connection error" << endl;  
  else cout << endl << "Client now connected" << endl; 

  //
  size_t msg_len = strlen(msg.c_str());
  int num_bytes = send(clnt_sock, msg.c_str(), msg_len, 0); 
  
  // send() returns the number of bytes actually sent out— might be less than what we told it to send
  if (num_bytes < 0) cout << endl << "Server: error in send() to server" << endl; 
  // else
  cout << endl << "Sent "<< num_bytes << " bytes to web server" << endl
       << "Header message size: " << msg_len << " bytes" << endl;

  // buffer storage 
  char buf[MAX_DATA_SIZE];
  // content types 
  #define TEXT 0
  #define NON_TEXT 1
  int content_type = NON_TEXT;
  // error checking for check_socket
  int n_tries = 0; 
  // conversion flag, not OK, yet
  int convert = -1;
  map<string,string> converted_resp; // make header map out of response

  // recv() returns the number of bytes actually read into the buffer, or -1 on error
  // (with errno set, accordingly.
  // recv() can return 0. This can mean only one thing: the remote side has closed the connection
  // on you! A return value of 0 is recv()'s way of letting you know this has occurred.
  while(check_socket(clnt_sock, 2)) // wait 2 secs
  {
    num_bytes = recv(clnt_sock, buf, MAX_DATA_SIZE, 0);
    
    if (num_bytes > 0) cout << endl << "Received " <<  num_bytes << " bytes" <<  endl;
    else if (num_bytes < 0) break; // error
    else if (++n_tries > 3) break; // too many requests

    // keep track of recieved data
    // Copy num_bytes of characters of buf
    resp_string += string(buf, num_bytes); 

    // Convert headers from response (only once)
    if (convert)
    {
      ++convert; // conversion OK now, flag      
      hdr_to_map(converted_resp, resp_string); 
      cout << "Content-type: " << converted_resp["content-type: "] << endl;

      vector<string> content_types = {"html", "xml", "text"};
      // if content is non-compressed, enter another branch
      for (auto t : content_types)
      {
        size_t find_word = converted_resp["content-type: "].find(t); 
       
        if (find_word != string::npos) content_type = TEXT; 
      }
    }
    // fyll byte-stream (icke html/txt/xml) .. content is non-text
    if (content_type == NON_TEXT)
    {
      num_bytes = send(cnnctr_sock, resp_string.c_str(), resp_string.size(), 0);	
      cout << endl << num_bytes << " bytes of non-text sent " << endl;
      resp_string = ""; 
    }
  }

  cout << "Content size: " << resp_string.size()  << endl;
  
  close(clnt_sock);
  
  //  content is txt/html/xml
  if (content_type == TEXT)
  {
    // check for content inside http headers
    cout << endl << "Searched headers for content inside of text" << endl;

    // do 302 redirect
    if (forbidden_words(resp_string))
    {
      cout << endl << "HTTP 302 Redirect (Bad Content)" << endl;
      
      // Redirect to error page
      resp_string =
        "HTTP/1.1 302 Found\r\nLocation: http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html";
      resp_string += "\r\nConnection: Close\r\n\r\n";
      
      // send() returns the number of bytes actually sent out
      num_bytes = send(cnnctr_sock, resp_string.c_str(), resp_string.size(), 0);

      cout << endl << "Redirect: " << num_bytes  << " bytes sent" << endl;

      // close if open
      if (cnnctr_sock) close(cnnctr_sock); 
      cout << endl << "Closed connector socket"  << endl;
      
      exit(0);  
    }
    // no redirect
    else
    {
      unsigned len = 0;

      // send until we reach content.size()
      while (len < resp_string.size())
      {
        // statistik
        num_bytes = send(cnnctr_sock, resp_string.c_str() + len, resp_string.size()-len, 0);
        len = len + num_bytes;
      }

      cout << endl << "Size to send: " << resp_string.size()           
           << ", sent: " << num_bytes << " bytes" << endl
           << "Data sent: " << resp_string.c_str() << endl;
      
      // open? close!
      if (cnnctr_sock) { close(cnnctr_sock); }
      
      cout << endl << "Closed connector socket" << endl;
    }
  }
} 


////////////////////////////////
////////// MAIN  //////////////
////////////////////////////////
int main(int argc, char *argv[])
{
  // for reaping of zombie processes
  struct sigaction sa;
  int yes=1;
  char s[INET6_ADDRSTRLEN]; //char yes='1'; on Solaris
  
  // user defines server port via command line
  int SRVR_PRT;
  
  // convert arguments to vector with C++ strings
  vector<string> arg(argv, argv+argc);

  // argc: n number of command line arguments (incl. program name)
  if (argc != 2)
  {
    cerr << "Usage: ./proxy PORTNUMBER" << endl;
    return 0;
  }

  // try converting string to integer 
  try
  {
    SRVR_PRT = stoi(arg[1]);
  }
  catch (exception& e)
  {
    cerr <<  endl <<  "Command line conversion error: ("
         << e.what() << ")" << endl;
    return false;
  }
  
  // create socket file descriptor, so that we can bind(fd) it and listen(fd) to it
  int srv_sock;
  if ((srv_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) // we want TCP/IP
  { cerr << "Could not create server socket" << endl; }

   // get rid of the "address already in use" error message
  setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
  
  // to deal with struct sockaddr, there is a parallel structure: 
  // struct sockaddr_in (for Internet) to be used with IPv4.
  // this structure makes it easy to reference elements of the socket address
  sockaddr_in srvr;
  srvr.sin_family = AF_UNSPEC; // or AF_INET
  srvr.sin_addr.s_addr = INADDR_ANY; // IP-adress 4 or 6  
  srvr.sin_port = htons(SRVR_PRT); // host to network short // short, network byte order
  // connect() wants a struct sockaddr*, you canl use a struct sockaddr_in and cast it at the last minute
  //   should be set to all zeros/ with the function memset().
  // sin_family corresponds to sa_family in a struct sockaddr and should be set to “AF_INET”.
  // Finally, the sin_port must be in Network Byte Order (by using htons()!)

  // re-cast the addr to fit bind()'s needs
//  int bind === ??? *****************
  if ((bind(srv_sock, (struct sockaddr*)&srvr, sizeof(srvr))) < 0)
  {
    cerr << endl << "Server: bind() error: " <<  errno << endl; 
    close(srv_sock);
    return 0;
  }

  // BACKLOG: how many pending connections queue will hold
  // incoming connections are going to wait in this queue until we accept() them
  // (see below) and this is the limit on how many can queue up
  if ((listen(srv_sock, BACKLOG)) == -1) cerr << "Server: listen() error :" << errno << endl;

  // reap zombie processes
  sa.sa_handler = sigchld_handler; 
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  if (sigaction(SIGCHLD, &sa, nullptr) == -1)
  {
    cerr << endl << "Error: sigaction" << endl;
    exit(1);
  }

  // needed below 
  socklen_t addrlen;
  // Informative output
  cout << endl << "Welcome! Server started at port: " << SRVR_PRT << endl; 
  cout << endl << "Waiting for connections... " << endl;

  // server will wait for a connection, accept() it, and fork() a child process to handle it.
  while (1)   // infinite loop until exit(0)
  {
    sockaddr_in connector_address; // connector's address information
    addrlen = sizeof(connector_address);
    // now accept an incoming connection:
    // accept() returns the newly connected socket descriptor, or -1 on error,  errno set appropriately
    // ready to communicate on socket descriptor new_fd
    // Because all of this code is in an infinite loop, we
    // will return to the accept statement to wait for the next connection.                     
    int cnnctr_sock = accept(srv_sock, (sockaddr*)&connector_address, &addrlen);

    // After a connection is established, call fork() to create a new process
    // The child process will close sockfd and call check_socket()
    // passing the new socket file descriptor as an argument.
    // this is the child process 
    if(!fork()) 
    {
      int n_tries = 0; 
      string cnnctr_cnt; // connector content

      int num_bytes; // recv():ed bytes
      char buf[MAX_DATA_SIZE];

      map<string, string> http_hdr; // structure for storing header
      string resp_string;    // response to be fetched below
      
      cout << endl << "Entering child process " << endl;
      
      // child doesn't need the listener
      close(srv_sock);     
   
      // retrieve content
      while(check_socket(cnnctr_sock, 1)) // timeout 1 sec
      {
        // check what they (browser) have to say
        // returns the number of bytes received, -1 on error
        num_bytes = recv(cnnctr_sock, buf, MAX_DATA_SIZE, 0);        

        if ( num_bytes > 0 ) cout << endl << "Bytes received: " <<  num_bytes << endl;
        else if (num_bytes < 0) break; // error
        else if (++n_tries > 3) break; // too many requests
       
        // keep track of content from recv() from connector socket
        // copies the first n characters from the array of characters pointed by buf
        cnnctr_cnt +=  string(buf, num_bytes); 
      }

      // parse content
      // change the http header of the browser's messsage
      // the header is different when talking to a proxy than a web server
      hdr_to_map(http_hdr, cnnctr_cnt);

      // show what we got
      cout << endl << "Parsed " << http_hdr.size() << " rows of headers" << endl
           << "Request field: " << http_hdr["request: "] << endl <<  "Body: " << endl;      

      // show what we got again
      for (auto h : http_hdr) cout << endl << h.first << h.second;
      

      // content is not to be filtered in this branch
       // function returns false if forbidden word is found in the request field
      if (!forbidden_words(http_hdr["request: "]))
      {
        cout << endl << "Searched URL for forbidden words, found none" <<  endl;

        // create new header for our needs, to send to conversation
        string hdr_msg = ""; 
        create_http_hdr(hdr_msg, http_hdr);

        cout << endl << "Server-directed message size: " << hdr_msg.size() << " bytes" <<  endl; 

        // call conversation and store the response string (reference)
        // send header to web server
        conversation(resp_string, hdr_msg, http_hdr["host: "], cnnctr_sock);
        
        cout << endl << "Got response" <<  endl;
      }
      // forbidden words in the URL - redirect
      else 
      {
        // HTTP/1.0 compatible redirect
        // HTTP/1.1 (RFC 2616) added the new status codes 303 and 307
        // in web frameworks to preserve compatibility with browsers
        // that do not implement the HTTP/1.1 specification
        cout << endl << "Searched URL for forbidden words, detected bad word"
             << endl << "HTTP 302 Redirect (Bad URL)" <<  endl;

        // Redirect to error page
        resp_string =
          "HTTP/1.1 302 Found\r\nLocation: http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error1.html";
        // add connection: close
         resp_string += "\r\nConnection: Close\r\n\r\n";

        int n_bytes = send(cnnctr_sock, resp_string.c_str(), resp_string.size(), 0);
        
        cout << endl << "Response: " << resp_string << endl << "Redirecting " << endl
             << n_bytes  << " bytes sent"  <<  endl;	
      }

      // close if open
      if (cnnctr_sock) close(cnnctr_sock); 
      
      cout << endl << "Closed connector socket" << endl;
      // Quit
      exit(0);  
    }
    //  When the two processes have completed their conversation, as indicated by check_socket()
    //  this process simply exits. The parent process closes the new sock fd   
    else close(cnnctr_sock);  // (fork())
  }
  // 
  close(srv_sock);
  // end program
  return 0;
} 
