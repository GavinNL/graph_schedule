#pragma once
#ifndef FRAME_GRAPH_2_H
#define FRAME_GRAPH_2_H

#include <memory>
#include <future>

#include <map>
#include <vector>
#include <queue>
#include <any>
#include <iostream>

#include <sstream>
#include <iostream>
#include <iomanip>

//uint32_t thread_count = 0;
//thread_local ThreadStreams TS;
//thread_local std::ostream & fout = TS.get_stream(  );
//thread_local uint32_t thread_number = thread_count++;

//#define FOUT fout << std::setw(8) << ThreadStreams::time().count() << ": " << std::string( 40*thread_number, ' ')
#define FOUT std::cout << ": " << std::this_thread::get_id() << ": "

class FrameGraph;
class ExecNode;
class ResourceNode;
using ExecNode_p = std::shared_ptr<ExecNode>;
using ResourceNode_p = std::shared_ptr<ResourceNode>;


/**
 * @brief The ExecNode class
 *
 * An ExecNode is a node that executes some kind of computation.
 */
class ExecNode
{
public:
    std::any     m_NodeClass;                      // an instance of the Node class
    std::any     m_NodeData;                       // an instance of the node data
    std::mutex   m_mutex;                          // mutex to prevent the node from executing twice
    bool         m_executed = false;               // flag to indicate whether the node has been executed.
    FrameGraph * m_Graph; // the parent graph;

    uint32_t     m_resourceCount = 0;

    std::vector<ResourceNode_p> m_requiredResources; // a list of required resources
    std::vector<ResourceNode_p> m_producedResources; // a list of required resources

    std::function<void(void)> execute; // Function object to execute the Node's () operator.


    // nudge the ExecNode to check whether all its resources are available.
    // if they are, then tell the FrameGraph to schedule its execution
    void trigger();

    bool can_execute() const;

};

/**
 * @brief The ResourceNode class
 * A resource node is a node which holds a resource and is created or consumed by an ExecNode.
 *
 * An ExecNode is executed when all it's required resources have become available.
 */
class ResourceNode
{
public:
    std::any                resource;
    std::string             name;
    std::vector<ExecNode_p> m_Nodes; // list of nodes that must be triggered
                                     // when resource becomes availabe
    bool m_is_available = false;

    void make_available()
    {
        m_is_available = true;
    }
    bool is_available() const
    {
        return m_is_available;
    }
    void clear()
    {
        m_is_available = false;
    }
};

template<typename T>
class Resource
{
public:
    ResourceNode_p m_node;
    T & get()
    {
        return std::any_cast<T&>(m_node->resource);
    }

    /**
     * @brief clear
     * Clears the resource by making it unavailable.
     * This does not actually delete the
     */
    void clear()
    {
        m_node->clear();
    }

    void make_available()
    {
        if( !m_node->is_available() )
        {
            m_node->make_available();

            // loop through all the exec nodes which require this resource
            // and trigger them.
            for(auto & n : m_node->m_Nodes)
            {
                n->trigger();
            }
        }
    }

    Resource & operator = ( T const & v)
    {
        std::any_cast<T&>(m_node->resource) = this;
        return *this;
    }

    operator T&()
    {
        return std::any_cast<T&>(m_node->resource);
    }

    void set(T const & x, bool make_avail=true)
    {
        std::any_cast<T&>(m_node->resource) = x;
        if( make_avail) make_available();

    }
};

template<typename T>
using Resource_p = std::shared_ptr< Resource<T> >;

class ResourceRegistry
{
    std::map<std::string, ResourceNode_p> & m_resources;
    std::vector<ResourceNode_p> & m_required_resources;
    ExecNode_p & m_Node;

    public:
        ResourceRegistry( ExecNode_p & node,
                          std::map<std::string, ResourceNode_p> & m,
                          std::vector<ResourceNode_p> & required_resources) :
            m_Node(node),
            m_resources(m),
            m_required_resources(required_resources)
        {

        }

        template<typename T>
        Resource<T> create_PromiseResource(const std::string & name)
        {
            if( m_resources.count(name) == 0 )
            {
                ResourceNode_p RN = std::make_shared< ResourceNode >();
                RN->resource      = std::make_any<T>();
                RN->name          = name;
                m_Node->m_producedResources.push_back(RN);
                m_resources[name] = RN;

                Resource<T> r;
                r.m_node = RN;

                return r;
            }
            else
            {
                Resource<T> r;
                r.m_node = m_resources.at(name);
                return r;
            }
        }

        template<typename T>
        Resource<T> create_FutureResource(const std::string & name)
        {
            if( m_resources.count(name) == 0 )
            {
                Resource_p<T> X = std::make_shared< Resource<T> >();

                ResourceNode_p RN = std::make_shared<ResourceNode>();
                RN->resource      = X;
                RN->m_Nodes.push_back(m_Node);
                RN->name = name;
                m_resources[name] = RN;

                Resource<T> r;
                r.m_node = RN;

                m_required_resources.push_back(RN);

                return r;
            }
            else
            {
                ResourceNode_p RN = m_resources[name];
                RN->m_Nodes.push_back(m_Node);

                Resource<T> r;
                r.m_node = RN;

                m_required_resources.push_back(RN);

                return r;
            }
        }
};


class FrameGraph
{
public:
    FrameGraph() {}
    ~FrameGraph()
    {
        m_cv.notify_all();

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]{return num_waiting==m_threads.size(); } ); // keep waiting if the queue is empty.
            quit = true;
        }
        m_cv.notify_all();
        for(auto & t : m_threads)
        {
            if(t.joinable())
            {
                t.join();
            }
        }

    }

    FrameGraph( FrameGraph const & other)
    {
        if( this != & other)
        {

        }
    }

    FrameGraph( FrameGraph && other)
    {
    }

    FrameGraph & operator = ( FrameGraph const & other)
    {
        if( this != & other)
        {

        }
        return *this;
    }

    FrameGraph & operator = ( FrameGraph && other)
    {
        if( this != & other)
        {

        }
        return *this;
    }

    template<typename _Tp, typename... _Args>
      inline void
      AddNode(_Args&&... __args)
      {
          typedef typename std::remove_const<_Tp>::type Node_t;
          typedef typename Node_t::Data_t               Data_t;

          ExecNode_p N   = std::make_shared<ExecNode>();

          N->m_NodeClass = std::make_any<Node_t>( std::forward<_Args>(__args)...);
          N->m_NodeData  = std::make_any<Data_t>();

          ExecNode* rawp = N.get();
          rawp->m_Graph = this;
          N->execute = [rawp]()
          {
              if( !rawp->m_executed ) // if we haven't executed yet.
              {

                  if( rawp->m_mutex.try_lock() ) // try to lock the mutex
                  {                              // if we have acquired the lock, execute the node
                      rawp->m_executed = true;
                      std::any_cast< Node_t&>( rawp->m_NodeClass )(   std::any_cast< Data_t&>( rawp->m_NodeData ) );
                      rawp->m_mutex.unlock();
                  }
              } else {
                  FOUT << "Already Executed." << std::endl;
              }
          };

          ResourceRegistry R(N,m_resources,
                             N->m_requiredResources);

          std::any_cast< Node_t&>(N->m_NodeClass).registerResources( std::any_cast< Data_t&>( rawp->m_NodeData ), R);

          m_execNodes.push_back(N);
      }

     /**
     * @brief append_node
     * @param node
     *
     * Append a node to the execution queue. so that it can be executed
     * when the next thread worker is available.
     */
    void append_node( ExecNode * node)
    {
        m_ToExecute.push(node);
        m_cv.notify_all();

    }

    /**
     * @brief execute_serial
     *
     * Executes the graph on a single thread.
     */
    void execute_serial()
    {
        for(auto & N : m_execNodes) // place all the nodes with no resource requirements onto the queue.
        {
            if( N->m_requiredResources.size() == 0)
            {
                m_ToExecute.push(N.get());
            }
        }

        // execute the all nodes in the queue.
        // New nodes will be added
        while( m_ToExecute.size() )
        {
            m_ToExecute.front()->execute();
            m_ToExecute.pop();
        }
    }

    //==================================================
    //
    //==================================================
    void execute_threaded(int n)
    {
        for(int i=0;i<n;i++)
        {
            m_threads.emplace_back(  std::thread(&FrameGraph::thread_worker, this));
        }
        for(auto & N : m_execNodes) // place all the nodes with no resource requirements onto the queue.
        {
            if( N->m_requiredResources.size() == 0)
            {
                m_ToExecute.push(N.get());
            }
        }
        m_cv.notify_all();
    }
    //==================================================

    /**
     * @brief clear_resources
     * CLears all the resources to their unavailable state.
     * This does not destroy the resource.
     */
    void clear_resources()
    {
        for(auto & r : m_resources)
        {
            r.second->clear();
        }
    }

    template<typename T>
    T & get_resource(std::string const & name)
    {
        return std::any_cast<T&>(m_resources.at(name));
    }

    void print()
    {
        std::cout << "digraph G {" << std::endl;

        uint32_t i=0;
        for(auto & E : m_execNodes)
        {
            std::cout << "Node_" << i << " [shape=Msquare]" << std::endl;
            for(auto & R : E->m_producedResources)
            {
                std::cout << "   Node_" << i << " -> " << R->name << std::endl;
            }
            for(auto & R : E->m_requiredResources)
            {
                std::cout << "   " << R->name << " -> " << "Node_" << i << std::endl;
            }
            i++;
        }

        std::cout << "}" << std::endl;

    }

    std::vector< ExecNode_p >             m_execNodes;
    std::map<std::string, ResourceNode_p> m_resources;

    // queue of all the nodes ready to be launched
    std::queue<ExecNode*>                 m_ToExecute;
    bool quit=false;

    std::vector< std::thread > m_threads;

    std::mutex              m_mutex;
    std::condition_variable m_cv;

    uint32_t num_running = 0;
    uint32_t num_waiting = 0;

    // Note to Self: Condition_varaible.wait( mutex ) will wait until condition_variable.notify_XXX() is called before it attempts to lock the mutex.
    //
    void  thread_worker()
    {
        while( true )
        {
            ExecNode * Job = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_mutex);

                num_waiting++;
                m_cv.wait(lock, [this]{return !m_ToExecute.empty() || quit; } ); // keep waiting if the queue is empty.
                num_waiting--;

                if(quit)
                {
                    break;
                }
                if( m_ToExecute.size() )
                {
                    Job = m_ToExecute.front();
                    m_ToExecute.pop();

                }
            }

            ++num_running;
            Job->execute();
            --num_running;

            m_cv.notify_all();
        }

        m_cv.notify_all();

    }




};

inline void ExecNode::trigger()
{
    ++m_resourceCount;
    if( m_resourceCount >= m_requiredResources.size())
    {
        m_Graph->append_node(this);
    }
}

bool ExecNode::can_execute() const
{
    for(auto & r : m_requiredResources)
    {
        if(!r->is_available())
        {
            return false;
        }
    }
    return true;
}
#endif

