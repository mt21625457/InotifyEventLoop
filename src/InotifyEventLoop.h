#ifndef __INOTIFY_EVENTLOOP_H__
#define __INOTIFY_EVENTLOOP_H__


/*
    inotify
    用于 linux  文件系统监控  
    兼容性：    linux内核版本大于 2.6
    测试系统：  ubuntu1104（内核版本 2.6.38）   uos20(内核版本4.19)
    版本：      0.1.0
*/

#include <string>
#include <map>
#include <list>
#include "EpollEvent.h"


#ifdef __FreeBSD__
#define stat64 stat
#define lstat64 lstat
#endif

/* the following are legal, implemented events that user-space can watch for */
#define IN_ACCESS		    0x00000001	/* File was accessed */
#define IN_MODIFY		    0x00000002	/* File was modified */
#define IN_ATTRIB		    0x00000004	/* Metadata changed */
#define IN_CLOSE_WRITE		0x00000008	/* Writtable file was closed */
#define IN_CLOSE_NOWRITE	0x00000010	/* Unwrittable file closed */
#define IN_OPEN			    0x00000020	/* File was opened */
#define IN_MOVED_FROM		0x00000040	/* File was moved from X */
#define IN_MOVED_TO		    0x00000080	/* File was moved to Y */
#define IN_CREATE		    0x00000100	/* Subfile was created */
#define IN_DELETE		    0x00000200	/* Subfile was deleted */
#define IN_DELETE_SELF		0x00000400	/* Self was deleted */
#define IN_MOVE_SELF		0x00000800	/* Self was moved */

/* the following are legal events.  they are sent as needed to any watch */
#define IN_UNMOUNT		    0x00002000	/* Backing fs was unmounted */
#define IN_Q_OVERFLOW		0x00004000	/* Event queued overflowed */
#define IN_IGNORED		    0x00008000	/* File was ignored */

/* helper events */
#define IN_CLOSE		(IN_CLOSE_WRITE | IN_CLOSE_NOWRITE) /* close */
#define IN_MOVE			(IN_MOVED_FROM | IN_MOVED_TO) /* moves */

/* special flags */
#define IN_ONLYDIR		    0x01000000	/* only watch the path if it is a directory */
#define IN_DONT_FOLLOW		0x02000000	/* don't follow a sym link */
#define IN_MASK_ADD		    0x20000000	/* add to the mask of an already existing watch */
#define IN_ISDIR		    0x40000000	/* event occurred against dir */
#define IN_ONESHOT		    0x80000000	/* only send event once */

/*
 * All of the events - we build the list by hand so that we can add flags in
 * the future and not break backward compatibility.  Apps will get only the
 * events that they originally wanted.  Be sure to add new events here!
 */
#define IN_ALL_EVENTS	(IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | \
			 IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | \
			 IN_MOVED_TO | IN_DELETE | IN_CREATE | IN_DELETE_SELF | \
			 IN_MOVE_SELF)

// #define IN_ALL_EVENTS	(IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | \
// 			 IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | \
// 			 IN_MOVED_TO | IN_DELETE | IN_CREATE )

#define INOTIFY_ROOT -9527

namespace inotify {

struct BlockNode {
    static BlockNode * create(int wd,int parent_wd,unsigned int events,const char * name,bool is_dir)
    {
        BlockNode * node = new BlockNode;
        node->wd        = wd;
        node->events    = events;
        node->name      = name;
        node->is_dir    = is_dir;
        node->parent_wd = parent_wd;
        return node;
    }

    void  add_child(int child)
    {
        this->child.push_back(child);
    }

    int                         wd;
    int                         parent_wd;
    unsigned                    events;
    std::string                 name;
    bool                        is_dir;
    std::list<int>              child;
};


struct InotifyEvent {
	int		        wd;		    /* watch descriptor */
	uint32_t		mask;		/* watch mask */
	uint32_t		cookie;		/* cookie to synchronize two events */
	uint32_t		len;		/* length (including nulls) of name */
	char		    name[0];	/* stub for possible name */
};


class InotifyEventLoop
{
public:
    InotifyEventLoop();
    ~InotifyEventLoop();

public:
    /*
    *   初始化
    * 
    *   return: true 成功，fales 失败
    *   通过 error(); 返回错误码
    */
    bool    init();

    /*
    *      array:  InotifyEvent的指针数组,  返回的 InotifyEvent指针不需要释放（切记）  input output
    *       size:  指针数据的大小            input
    *  exception:  用于异常处理，待完善       output
    *     return:  返回读到的事件数量    成功： > 0   失败 <= 0     
    */
    int     read_event(InotifyEvent * array[], uint16_t size,int * exception);

    /*
    *    用于所有的监控wd的清理，会清空目录树，但不会close inotify fd
    *    清空后，可以继续添加目录或者文件进行监控
    */
    void    clear();
    
    /*
    *   待完善
    *   用于返回错误码，后续会以枚举的方式完善
    *   return: 错误码  
    */
    int     error();

    /*
    *   将一个文件或者目录添加到监控， 不会监控目录下的所有文件和目录
    *       file:  文件名           input
    *     events:  监控的事件       input
    *     return:   true 成功，fales 失败    
    * */
    bool    add_watch_file(const char * file,unsigned int events);

    /*
    *   将多个文件或者目录添加到监控，不会监控目录下的所有文件和目录
    *       file:  文件名           input
    *     events:  监控的事件       input
    *     return:   true 成功，fales 失败    
    * */
    bool    add_watch_files(const char * files[],unsigned int size,unsigned int events);
    
    /*
    *   将多个文件或者目录添加到监控，会监控目录下的所有文件和目录，非递归实现，效率高，放心使用，栈不会炸
    *       file:  文件名           input
    *     events:  监控的事件       input
    *     return:   true 成功，fales 失败    
    * */
    bool    add_watch_recursively( const char * path, unsigned int events);

    /*
    *   判断文件是目录或者文件
    *       file:  文件名       input
    *     return： 1：目录  0：文件 其他：错误  
    */
    int     is_dir(const char * path);
    
    /*
    *       将wd移出监控
    *         wd:  事件的wd  input
    *       
    * */
    void    remove_watch_wd(int wd);
    
    /*
    *   从目录树中返回 监控文件的完整路径
    *        wd:  监控文件的wd   input
    *      path:  返回输出的路径  input output  
    *    return:   true 成功，fales 失败   
    * */
    bool    get_path(int wd,std::string & path);

    /*
    *    返回inotify 的  fd ,可用于epoll 等多路IO 进行异步处理
    *   
    *     
    *    return:   true 成功，fales 失败   
    * */
    int     get_inotify_fd();

private: 
    /* 内部 处理 */
    bool        add_watch_block_file_recursively(int parent_wd,const char * path, unsigned int events);   
    int         add_watch_block_file(int parent_wd,const char * file,const char * name,unsigned int events,bool is_dir);
    int         get_child_wd(BlockNode*node,std::string name);
    bool        watch_block_insert(BlockNode* node);
    BlockNode * watch_block_search(int wd);

private:
    int                             m_inotify_fd;
    int                             m_error;
    bool                            m_init;

    char                    *       m_event_buffer;
    uint16_t                        m_event_buffer_size;

    bool                            m_is_recursively;
    bool 		                    m_moved_from;
    BlockNode *                     m_moved_from_node;

    std::map<int,BlockNode*>        m_block_map;
    datacenter::Event*              m_epoll_event;
};



}//namespace inotify

#endif