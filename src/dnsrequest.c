#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include<stdio.h> 
#include<string.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<unistd.h>
#include<time.h> 

void ChangetoDnsNameFormat(unsigned char*, unsigned char*);

static const char *config_keys[] =
{
        "Interface",
        "Server",
        "Hostname"
};
static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);

char* interface;
char* server;
char* hostname;
//Types of DNS resource records :)

#define T_A 1 //Ipv4 address
#define T_NS 2 //Nameserver
#define T_CNAME 5 // canonical name
#define T_SOA 6 /* start of authority zone */
#define T_PTR 12 /* domain name pointer */
#define T_MX 15 //Mail server

//DNS header structure
struct DNS_HEADER
{
	unsigned short id; // identification number

	unsigned char rd :1; // recursion desired
	unsigned char tc :1; // truncated message
	unsigned char aa :1; // authoritive answer
	unsigned char opcode :4; // purpose of message
	unsigned char qr :1; // query/response flag

	unsigned char rcode :4; // response code
	unsigned char cd :1; // checking disabled
	unsigned char ad :1; // authenticated data
	unsigned char z :1; // its z! reserved
	unsigned char ra :1; // recursion available

	unsigned short q_count; // number of question entries
	unsigned short ans_count; // number of answer entries
	unsigned short auth_count; // number of authority entries
	unsigned short add_count; // number of resource entries
};

//Constant sized fields of query structure
struct QUESTION
{
	unsigned short qtype;
	unsigned short qclass;
};

//Constant sized fields of the resource record structure
#pragma pack(push, 1)
struct R_DATA
{
	unsigned short type;
	unsigned short _class;
	unsigned int ttl;
	unsigned short data_len;
};
#pragma pack(pop)

//Pointers to resource record contents
struct RES_RECORD
{
	unsigned char *name;
	struct R_DATA *resource;
	unsigned char *rdata;
};

//Structure of a Query
typedef struct
{
	unsigned char *name;
	struct QUESTION *ques;
} QUERY;

static int dns_request_config (const char *key, const char *value)
{
	if (strcasecmp (key, "Interface") == 0)
	{
		strcpy(interface, value);
		return (1);
	}
	else if (strcasecmp (key, "Server") == 0)
	{
		if (value != NULL)
			strcpy (server, value);
	}
	else if (strcasecmp (key, "Hostname") == 0)
	{
		if (value != NULL)
			strcpy(hostname, value);
	}
	else
	{
		return (-1);
	}

	return (0);
}

static void submit_rtt (float value)
{
	value_t values[1];
        value_list_t vl = VALUE_LIST_INIT;

        values[0].gauge = value;

        vl.values = values;
        vl.values_len = 1;
        sstrncpy (vl.host, hostname_g, sizeof (vl.host));
	sstrncpy (vl.plugin, "dns_request", sizeof (vl.plugin));
	sstrncpy (vl.type, "dns_rtt", sizeof (vl.type));
	plugin_dispatch_values (&vl);
} /* void submit_rtt */

static int dns_request_read (void)
{

	//unsigned char hostname[100];
	int query_type = T_A;
	unsigned char buf[65536],*qname;
	int i , s;


	struct sockaddr_in dest;

	struct DNS_HEADER *dns = NULL;
	struct QUESTION *qinfo = NULL;

	s = socket(AF_INET , SOCK_DGRAM , IPPROTO_UDP); //UDP packet for DNS queries
	setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, interface, sizeof(interface));

	dest.sin_family = AF_INET;
	dest.sin_port = htons(53);
	dest.sin_addr.s_addr = inet_addr(server);

	//Set the DNS structure to standard queries
	dns = (struct DNS_HEADER *)&buf;

	dns->id = (unsigned short) htons(getpid());
	dns->qr = 0; //This is a query
	dns->opcode = 0; //This is a standard query
	dns->aa = 0; //Not Authoritative
	dns->tc = 0; //This message is not truncated
	dns->rd = 1; //Recursion Desired
	dns->ra = 0; //Recursion not available! hey we dont have it (lol)
	dns->z = 0;
	dns->ad = 0;
	dns->cd = 0;
	dns->rcode = 0;
	dns->q_count = htons(1); //we have only 1 question
	dns->ans_count = 0;
	dns->auth_count = 0;
	dns->add_count = 0;

	//point to the query portion
	qname =(unsigned char*)&buf[sizeof(struct DNS_HEADER)];

	ChangetoDnsNameFormat(qname , (unsigned char*)&hostname);
	qinfo =(struct QUESTION*)&buf[sizeof(struct DNS_HEADER) + (strlen((const char*)qname) + 1)]; //fill it

	qinfo->qtype = htons( query_type ); //type of the query , A , MX , CNAME , NS etc
	qinfo->qclass = htons(1); //its internet (lol)

	clock_t start = clock();

	if( sendto(s,(char*)buf,sizeof(struct DNS_HEADER) + (strlen((const char*)qname)+1) + sizeof(struct QUESTION),0,(struct sockaddr*)&dest,sizeof(dest)) < 0)
	{
		DEBUG("sendto failed");
	}

	//Receive the answer
	i = sizeof dest;
	if(recvfrom (s,(char*)buf , 65536 , 0 , (struct sockaddr*)&dest , (socklen_t*)&i ) < 0)
	{
		DEBUG("recvfrom failed");
	}
	clock_t end = clock();
	float seconds = (float)(end - start) / CLOCKS_PER_SEC; 

	dns = (struct DNS_HEADER*) buf;

	if (ntohs(dns->ans_count) > 0) {
		submit_rtt(seconds);
	} else {
		DEBUG("response does not contain any answers.");
	}
	return (0);

}

static int dns_request_init (void)
{
	hostname = malloc(sizeof(char)*24);
	server = malloc(sizeof(char)*16);
	interface = malloc(sizeof(char)*8);
	return (0);
} /* int dns_init */


void module_register (void)
{
	plugin_register_config ("dns_request", dns_request_config, config_keys, config_keys_num);
	plugin_register_init ("dns_request", dns_request_init);
	plugin_register_read ("dns_request", dns_request_read);
} /* void module_register */

/*
 * This will convert www.google.com to 3www6google3com
 * */
void ChangetoDnsNameFormat(unsigned char* dns, unsigned char* host)
{
	int lock = 0 , i;
	strcat((char*)host,".");

	for(i = 0 ; i < strlen((char*)host) ; i++)
	{
		if(host[i]=='.')
		{
			*dns++ = i-lock;
			for(;lock<i;lock++)
			{
				*dns++=host[lock];
			}
			lock++; //or lock=i+1;
		}
	}
	*dns++='\0';
}
