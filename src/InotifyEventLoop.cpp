#include "InotifyEventLoop.h"

extern "C" {
	#include <sys/syscall.h>
	#include <string.h>
	#include <strings.h>
	#include <stdlib.h>
	#include <stdint.h>
	#include <stdio.h>
	#include <errno.h>
	#include <sys/select.h>
	#include <sys/types.h>
	#include <sys/stat.h>
	#include <sys/ioctl.h>
	#include <unistd.h>
	#include <dirent.h>
	#include <time.h>
	#include <regex.h>
	#include <setjmp.h>
}

#include <stack>
#include <iostream>


#ifdef __FreeBSD__
#define stat64 stat
#define lstat64 lstat
#endif

/*
 * struct inotify_event - structure read from the inotify device for each event
 *
 * When you are watching a directory, you will receive the filename for events
 * such as IN_CREATE, IN_DELETE, IN_OPEN, IN_CLOSE, ..., relative to the wd.
 */


#if defined (__alpha__)
# define __NR_inotify_init 444
# define __NR_inotify_add_watch 445
# define __NR_inotify_rm_watch 446

#elif defined (__arm__)
# define __NR_inotify_init (__NR_SYSCALL_BASE+316)
# define __NR_inotify_add_watch (__NR_SYSCALL_BASE+317)
# define __NR_inotify_rm_watch (__NR_SYSCALL_BASE+318)

#elif defined (__frv__)
# define __NR_inotify_init 291
# define __NR_inotify_add_watch 292
# define __NR_inotify_rm_watch 293

#elif defined(__i386__)
# define __NR_inotify_init 291
# define __NR_inotify_add_watch 292
# define __NR_inotify_rm_watch 293

#elif defined (__ia64__)
# define __NR_inotify_init 1277
# define __NR_inotify_add_watch 1278
# define __NR_inotify_rm_watch 1279

#elif defined (__mips__)
# if _MIPS_SIM == _MIPS_SIM_ABI32
#  define __NR_inotify_init (__NR_Linux + 284)
#  define __NR_inotify_add_watch (__NR_Linux + 285)
#  define __NR_inotify_rm_watch (__NR_Linux + 286)
# endif
# if _MIPS_SIM == _MIPS_SIM_ABI64
#  define __NR_inotify_init (__NR_Linux + 243)
#  define __NR_inotify_add_watch (__NR_Linux + 244)
#  define __NR_inotify_rm_watch (__NR_Linux + 245)
# endif
# if _MIPS_SIM == _MIPS_SIM_NABI32
#  define __NR_inotify_init (__NR_Linux + 247)
#  define __NR_inotify_add_watch (__NR_Linux + 248)
#  define __NR_inotify_rm_watch (__NR_Linux + 249)
# endif

#elif defined(__parisc__)
# define __NR_inotify_init (__NR_Linux + 269)
# define __NR_inotify_add_watch (__NR_Linux + 270)
# define __NR_inotify_rm_watch (__NR_Linux + 271)

#elif defined(__powerpc__) || defined(__powerpc64__)
# define __NR_inotify_init 275
# define __NR_inotify_add_watch 276
# define __NR_inotify_rm_watch 277

#elif defined (__s390__)
# define __NR_inotify_init 284
# define __NR_inotify_add_watch 285
# define __NR_inotify_rm_watch 286

#elif defined (__sh__)
# define __NR_inotify_init 290
# define __NR_inotify_add_watch 291
# define __NR_inotify_rm_watch 292

#elif defined (__sh64__)
# define __NR_inotify_init 318
# define __NR_inotify_add_watch 319
# define __NR_inotify_rm_watch 320

#elif defined (__sparc__) || defined (__sparc64__)
# define __NR_inotify_init 151
# define __NR_inotify_add_watch 152
# define __NR_inotify_rm_watch 156

#elif defined(__x86_64__)
# define __NR_inotify_init 253
# define __NR_inotify_add_watch 254
# define __NR_inotify_rm_watch 255

#else
# error "Unsupported architecture!"
#endif

static inline int inotify_init (void)
{
	return syscall (__NR_inotify_init);
}

static inline int inotify_add_watch (int fd, const char *name, uint32_t mask)
{
	return syscall (__NR_inotify_add_watch, fd, name, mask);
}

static inline int inotify_rm_watch (int fd, uint32_t wd)
{
	return syscall (__NR_inotify_rm_watch, fd, wd);
}


namespace inotify {

InotifyEventLoop::InotifyEventLoop()
{
	this->m_init			= false;
	this->m_error 			= 0;
	this->m_inotify_fd 		= -1;

	this->m_event_buffer_size 	= 8192;
	this->m_event_buffer		= (char *)calloc(8192,sizeof(char));
	this->m_is_recursively		= false;
	
	this->m_moved_from 		= false;
    this->m_moved_from_node = NULL;
}


InotifyEventLoop::~InotifyEventLoop()
{
	if(this->m_inotify_fd != -1)
	{
		close(this->m_inotify_fd);
		this->m_inotify_fd = -1;
	}

	if(this->m_epoll_event != NULL) {
		delete this->m_epoll_event;
		this->m_epoll_event = NULL;
	}

	if(this->m_event_buffer == NULL) {
		free(this->m_event_buffer);
	}
}

bool InotifyEventLoop::init() 
{
	if(this->m_init == true) {
		return true;
	} 

	bool is_ok = false;
	if (this->m_init) {
		return true;
	}
	
	this->m_error = 0;

	this->m_inotify_fd = inotify_init();
	if (this->m_inotify_fd < 0)	{
		this->m_error = errno;
		goto __ERROR;
	}

	this->m_init = true;
	return true;

__ERROR:
	if(this->m_inotify_fd != -1) {
		close(this->m_inotify_fd);
		this->m_inotify_fd = -1;
	}

	return false;
}


int  InotifyEventLoop::read_event(InotifyEvent * array[], uint16_t size,int * exception)
{
	unsigned int events = -1;

	int number = 0;
	char * 	pbuf = NULL;
	struct InotifyEvent * event = NULL;
	std::string path;

	
	unsigned int bytes_to_read;
	int rc = -1;
	do {
		rc = ioctl( this->m_inotify_fd, FIONREAD, &bytes_to_read );
	} while ( !rc &&
	          bytes_to_read < sizeof(struct InotifyEvent) );

	if ( rc == -1 ) {
		this->m_error = errno;
		return false;
	}

	memset(this->m_event_buffer,0,this->m_event_buffer_size);
	int  count = read(this->m_inotify_fd,this->m_event_buffer,this->m_event_buffer_size);
	if ( size <= 0 ) {
		this->m_error = errno;
		return count;
	}
	
	for(pbuf = this->m_event_buffer; pbuf < this->m_event_buffer + count;)
	{
		event = (struct InotifyEvent *)pbuf;
		if(this->m_moved_from && !(event->mask & IN_MOVED_TO))
		{
			if(this->m_moved_from_node != NULL) {
				this->remove_watch_wd(this->m_moved_from_node->wd);
			}
			
			this->m_moved_from_node = NULL;
			this->m_moved_from		= false;
		}

		if(event->mask & IN_DELETE_SELF) {
			this->remove_watch_wd(event->wd);
		}

		if(this->m_is_recursively)  
		{
			if ( (event->mask & IN_CREATE) ||
                ( !(this->m_moved_from) && (event->mask & IN_MOVED_TO)) ) 
			{
				bool is_ok = this->get_path(event->wd,path);
				if(is_ok == true) {
					BlockNode * node = this->watch_block_search(event->wd);
					if(node != NULL) {
						events = node->events;
					} else {
						events = IN_ALL_EVENTS;
					}
					
					path.append(event->name);
					int ret = this->is_dir(path.c_str());
					switch (ret)
					{
						case 0: this->add_watch_block_file(event->wd,path.c_str(),event->name,events,false); break;
						case 1: this->add_watch_recursively(path.c_str(),events); break;
						default:break;
					}
				} else {
				
					
				}
			}
			else if(event->mask & IN_MOVED_FROM ) 
			{
				BlockNode * node =  this->watch_block_search(event->wd);
				if(node != NULL) 
				{
					int wd = this->get_child_wd(node,event->name);
					if(wd != -1) {
						BlockNode * node1 = this->watch_block_search(wd);
						if(node1 != NULL) {
							this->m_moved_from_node = node1;
							this->m_moved_from 		= true;
						}
						else 
						{
							//std::cout<< "<==---IN_MOVED_FROM: watch_block_search child wd for node == NULL wd = "<< wd  <<std::endl;
						}
					} 
					else 
					{
						//std::cout<< "<==---IN_MOVED_FROM: get_child_wd failed name = " << event->name <<std::endl;
					}
				}
				else 
				{
					//std::cout<<"<==---IN_MOVED_FROM: get moved_from node failed wd = " << event->wd << std::endl;
				}
			}

			else if (event->mask & IN_MOVED_TO)
			{	
				if(this->m_moved_from && this->m_moved_from_node != NULL) {
					m_moved_from_node->parent_wd 	= event->wd;
					m_moved_from_node->name 		= event->name;
				}

				this->m_moved_from			= false;
				this->m_moved_from_node 	= NULL;
			}

		}
	
		array[number] = event;
		number++;
		path.clear();

		pbuf += sizeof(struct InotifyEvent) + event->len;
	}

	return number;
}


void  InotifyEventLoop::clear()
{
	BlockNode * node = NULL;
	std::map<int,BlockNode*>::iterator iter;
	for(iter = this->m_block_map.begin(); iter != this->m_block_map.end(); )
	{
		inotify_rm_watch(this->m_inotify_fd,iter->first);
		node = (BlockNode *)iter->second;
		delete node;
		this->m_block_map.erase(iter++);
	}

	this->m_is_recursively = false;
}

int InotifyEventLoop::error() {
	return this->m_error;
}

bool InotifyEventLoop::add_watch_file(const char * file,unsigned int events)
{
	if(file == NULL || this->m_init == false) {
		return false;
	}

	int wd = inotify_add_watch( this->m_inotify_fd, file, events );
	if(wd < 0) {
		this->m_error = errno;
		return false;
	}

	int ret =  this->is_dir(file);
	BlockNode * node = NULL;
	switch (ret)
	{
	case 0: node = BlockNode::create(wd,INOTIFY_ROOT,events,file,false);
			return watch_block_insert(node);
	case 1: node = BlockNode::create(wd,INOTIFY_ROOT,events,file,true);
			return watch_block_insert(node);
	default:
		return false;
	}	

	return false;
}

bool InotifyEventLoop::add_watch_files(const char * files[],unsigned int size,unsigned int events)
{
	if(this->m_init != true) {
		return false;
	}

	for(unsigned int i = 0; i < size && files[i] ; ++ i )
	{
		bool is_ok =  add_watch_file(files[i],events);
		if(is_ok == false ) {
			return false;
		}
	}
	
	return true;
}

bool InotifyEventLoop::add_watch_recursively( const  char * path,unsigned int events )
{
	if(path == NULL || this->m_init != true || this->is_dir(path) != 1) {
		return false;
	}

	return add_watch_block_file_recursively(INOTIFY_ROOT,path,events) ;
}

int InotifyEventLoop::is_dir( char const * path ) 
{
	static struct stat64 my_stat;
	if ( -1 == lstat64( path, &my_stat ) ) {
		if (errno == ENOENT) {
			this->m_error = ENOENT;
			return -1;
		} else {
			this->m_error = errno;
			return -2;
		}	
	}

	if( S_ISDIR( my_stat.st_mode ) && !S_ISLNK( my_stat.st_mode ) )
	{
		return 1;
	} else {
		return 0;
	}
}

void  InotifyEventLoop::remove_watch_wd(int wd)
{
	BlockNode * node = watch_block_search(wd);
	if(node != NULL) {

		if(node->parent_wd != INOTIFY_ROOT) 
		{
			BlockNode * parent_node = watch_block_search(node->parent_wd);
			if(parent_node != NULL) {
				std::list<int>::iterator iter;
				for(iter = parent_node->child.begin();iter != parent_node->child.end(); iter++)
				{
					if( (*iter) == wd ) {
						parent_node->child.erase(iter);
						break;
					}
				}
			}
		}
	
		delete node;
		inotify_rm_watch(this->m_inotify_fd,wd);
	}
}

int InotifyEventLoop::get_child_wd(BlockNode*node,std::string name)
{
	if(node == NULL) {
		return -1;
	}

	std::list<int>::iterator iter;
	for(iter = node->child.begin();iter != node->child.end(); iter++)
	{
		BlockNode*node = watch_block_search(*iter);
		if(node == NULL) {
			continue;
		}

		if( node->name.find(name) !=  std::string::npos)
		{
			return *iter;
		}
	}

	return -1;
}

bool  InotifyEventLoop::get_path(int wd,std::string & path)
{
	std::stack<std::string> _stack;
	int tmp_wd = wd;
	BlockNode * node  = NULL;
	bool is_dir = false;

	node =  watch_block_search(wd);
	if(node != NULL) {
		is_dir = node->is_dir;
	} else {
		return false;
	}

	do {
		node =  watch_block_search(tmp_wd);
		if(node == NULL) {
			return false;
		}
		_stack.push(node->name);
		tmp_wd = node->parent_wd;
	}while(node->parent_wd != INOTIFY_ROOT);

	while(!_stack.empty())
	{
		path += _stack.top();
		_stack.pop();

		if(!_stack.empty()) {
			if(path.at(path.length() -1) != '/') {
				path.append("/");
			}
		} else {
			if( is_dir && path.at(path.length() -1) != '/') {
				path.append("/");
			}
			break;
		}
	}

	return true;
}

int InotifyEventLoop::get_inotify_fd()
{
	return this->m_inotify_fd;
}

bool InotifyEventLoop::add_watch_block_file_recursively(int parent,const char * path, unsigned int events)
{
		if(path == NULL || this->m_init != true || this->is_dir(path) != 1) {
		return false;
	}

	BlockNode * node 	= NULL;
	static int first 	= 0;
	int ret 			= -1;
	DIR * dir 			= NULL;
	int parent_wd_tmp 	= -1;
	int parent_wd 		= -1;

	std::string _dir 		= path;
	std::stack<std::string> _stack;
	std::stack<int>			_statck_parent_wd;

	struct dirent * ent = NULL;
	bool is_ok = false;

	parent_wd = add_watch_block_file(parent,path,path,events,true);
	if(parent_wd == -1) {
		return false;
	}

	_stack.push(_dir);
	_statck_parent_wd.push(parent_wd);

	do {	
		std::string file_tmp;
		file_tmp = _stack.top();
		_stack.pop();

		parent_wd = _statck_parent_wd.top();
		_statck_parent_wd.pop();

		if( file_tmp.at(file_tmp.length() -1) != '/')
		{
			file_tmp.append("/");
		}
		
		dir = opendir(file_tmp.c_str());
		if(dir == NULL) {
			this->m_error = errno;
			return false;
		} 

		while(1) {
			std::string tmp;
			ent = readdir( dir );
			if(ent != NULL) 
			{
				switch(ent->d_type) 
				{
					case DT_REG:
						tmp = file_tmp;
						tmp.append(ent->d_name);
						ret = add_watch_block_file(parent_wd,tmp.c_str(),ent->d_name,events,false);
						if(ret == -1) {
							return false;
						}

						node = watch_block_search(parent_wd);
						if(node != NULL) {
							node->add_child(ret);
						}
						break;

					case DT_DIR:
						if( (0 != strcmp( ent->d_name, "." )) &&
		     				(0 != strcmp( ent->d_name, ".." )) )
						{
							tmp = file_tmp;
							tmp.append(ent->d_name);
							if(tmp.at(tmp.length() -1) != '/') {
								tmp.append("/");
							}
							_stack.push(tmp);
							parent_wd_tmp = add_watch_block_file(parent_wd,tmp.c_str(),ent->d_name,events,true);
							if(parent_wd_tmp == -1) {
								return false;
							}
							_statck_parent_wd.push(parent_wd_tmp);

							node = watch_block_search(parent_wd);
							if(node != NULL) {
								node->add_child(parent_wd_tmp);
							}
						}
						break;
					default: break;
				}
			}else {
				break;
			}
		}

		closedir(dir);

	}while( !_stack.empty());

	this->m_is_recursively = true;

	return true;
}


int InotifyEventLoop::add_watch_block_file(int parent_wd,const char * file,const char * name,unsigned int events,bool is_dir)
{
	if(file == NULL || this->m_init == false) {
		return -1;
	}

	int wd = inotify_add_watch( this->m_inotify_fd, file, events);
	if(wd < 0) {
		this->m_error = errno;
		return -1;
	}

	BlockNode * node = NULL;
	node = BlockNode::create(wd,parent_wd,events,name,is_dir);
	bool is_ok = watch_block_insert(node);
	if(is_ok == false) {
		return -1;
	}

	return wd;
}


bool  InotifyEventLoop::watch_block_insert(BlockNode* node)
{	
	if(node == NULL) 
	{
		return false;
	}

	if(node->parent_wd != INOTIFY_ROOT) 
	{
		if( this->watch_block_search(node->parent_wd) == NULL) 
		{
			return false;
		}
	}

	std::pair< std::map<int,BlockNode*>::iterator,bool > ret;

	ret = this->m_block_map.insert( std::pair<int,BlockNode*>(node->wd,node) ) ;
	if(ret.second == true ) {
		return true;
	} else {
		return false;
	}

}


BlockNode * InotifyEventLoop::watch_block_search(int wd)
{
	BlockNode * node = NULL;
	std::map<int,BlockNode*>::iterator iter =  this->m_block_map.find(wd);
	if( iter != this->m_block_map.end() ) {
		node = (BlockNode *)iter->second;
		return node;
	}else {
		return NULL;
	}
}


}//namespace inotify

