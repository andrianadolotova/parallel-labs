#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace std;

constexpr int PORT = 8080;
constexpr char ROOT_DIR[] = "static";

string readFile(const string& p){
    ifstream f(p, ios::binary);
    if(!f)return "";
    ostringstream s;s<<f.rdbuf();return s.str();
}

void sendResp(int c,string st,string b){
    ostringstream r;
    r<<"HTTP/1.1 "<<st<<"\r\nContent-Length:"<<b.size()
     <<"\r\nConnection: close\r\n\r\n"<<b;
    string x=r.str();
    send(c,x.c_str(),x.size(),0);
}

void handleClient(int c){
    char buf[4096];
    int n=recv(c,buf,4095,0);
    if(n<=0){close(c);return;}
    buf[n]=0;

    string m,p,v;
    istringstream ss(buf);
    ss>>m>>p>>v;

    if(p=="/")p="/index.html";

    string fp=string(ROOT_DIR)+p;
    string data=readFile(fp);

    if(data.empty())sendResp(c,"404 Not Found","<h1>404 Not Found</h1>");
    else sendResp(c,"200 OK",data);

    close(c);
}

int main(){
    int s=socket(AF_INET,SOCK_STREAM,0);

    sockaddr_in a{};
    a.sin_family=AF_INET;
    a.sin_addr.s_addr=INADDR_ANY;
    a.sin_port=htons(PORT);

    if(::bind(s,(sockaddr*)&a,sizeof(a))<0){
        perror("bind");
        return 1;
    }
    if(::listen(s,128)<0){
        perror("listen");
        return 1;
    }

    cout<<"RUN http://localhost:"<<PORT<<endl;

    while(true){
        int c=::accept(s,nullptr,nullptr);
        if(c<0){perror("accept");continue;}
        thread(handleClient,c).detach();
    }
}
