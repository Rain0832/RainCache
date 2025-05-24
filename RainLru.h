#pragma once

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "RainCache.h"

namespace RainCache
{
  // 前向声明
  template <typename Key, typename Value>
  class RainLru;

  template <typename Key, typename Value>
  class LruNode
  {
  private:
    Key key_;
    Value value_;
    size_t accessCount_;                        // 访问次数
    std::shared_ptr<LruNode<Key, Value>> next_; // 智能管理指针空间
    std::weak_ptr<LruNode<Key, Value>> prev_;   // 防止循环引用

  public:
    explicit LruNode(Key key, Value value)
        : key_(key),
          value_(value),
          accessCount_(1)
    {
    }

    // 提供必要的访问器
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value &value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class RainLru<Key, Value>;
  };

  template <typename Key, typename Value>
  class RainLru : public RainCache<Key, Value>
  {
  public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    explicit RainLru(int capacity)
        : capacity_(capacity)
    {
      initializeList();
    }

    ~RainLru() override = default;

    // 添加缓存
    void put(Key key, Value value) override
    {
      if (capacity_ <= 0)
        return;

      std::lock_guard<std::mutex> lock(mutex_);
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
        // 如果在当前容器中,则更新 value,并调用 get 方法，代表该数据刚被访问
        updateExistingNode(it->second, value);
        return;
      }

      addNewNode(key, value);
    }

    // 查询缓存，传出参数
    bool get(Key key, Value &value) override
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
        moveToMostRecent(it->second);
        value = it->second->getValue();
        return true;
      }
      return false;
    }

    // 查询缓存，返回值
    Value get(Key key) override
    {
      Value value{};
      get(key, value);
      return value;
    }

    // 删除指定元素
    void remove(Key key)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
        removeNode(it->second);
        nodeMap_.erase(it);
      }
    }

  private:
    // 初始化链表
    void initializeList()
    {
      // 创建首尾虚拟节点
      dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
      dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
      dummyHead_->next_ = dummyTail_;
      dummyTail_->prev_ = dummyHead_;
    }

    // 更新已存在缓存节点
    void updateExistingNode(NodePtr node, const Value &value)
    {
      node->setValue(value);
      moveToMostRecent(node);
    }

    // 添加新的缓存节点
    void addNewNode(const Key &key, const Value &value)
    {
      if (nodeMap_.size() >= capacity_)
      {
        evictLeastRecent();
      }

      NodePtr newNode = std::make_shared<LruNodeType>(key, value);
      insertNode(newNode);
      nodeMap_[key] = newNode;
    }

    // 将节点移动到最新的位置
    void moveToMostRecent(NodePtr node)
    {
      removeNode(node);
      insertNode(node);
    }

    // 移除一个节点
    void removeNode(NodePtr node)
    {
      if (!node->prev_.expired() && node->next_)
      {
        auto prev = node->prev_.lock(); // 使用lock()获取shared_ptr
        prev->next_ = node->next_;
        node->next_->prev_ = prev;
        node->next_ = nullptr; // 清空next_指针，彻底断开节点与链表的连接
      }
    }

    // 从尾部插入结点
    void insertNode(NodePtr node)
    {
      node->next_ = dummyTail_;
      node->prev_ = dummyTail_->prev_;
      dummyTail_->prev_.lock()->next_ = node; // 使用lock()获取shared_ptr
      dummyTail_->prev_ = node;
    }

    // 驱逐最近最少访问
    void evictLeastRecent()
    {
      NodePtr leastRecent = dummyHead_->next_;
      removeNode(leastRecent);
      nodeMap_.erase(leastRecent->getKey());
    }

  private:
    int capacity_;      // 缓存容量
    NodeMap nodeMap_;   // key -> Node
    std::mutex mutex_;  // 互斥锁
    NodePtr dummyHead_; // 虚拟头结点
    NodePtr dummyTail_; // 虚拟尾结点
  };

  // LRU-k 优化，继承 LRU 类
  template <typename Key, typename Value>
  class RainLruK : public RainLru<Key, Value>
  {

  public:
    explicit RainLruK(int capacity, int historyCapacity, int k)
        : RainLru<Key, Value>(capacity),
          historyList_(std::make_unique<RainLru<Key, size_t>>(historyCapacity)), // 调用父类构造
          k_(k)
    {
    }

    // get 接口 返回值
    Value get(Key key)
    {
      // 首先尝试从主缓存获取数据
      Value value{};
      bool inMainCache = RainLru<Key, Value>::get(key, value);

      // 获取并更新访问历史计数
      size_t historyCount = historyList_->get(key);
      historyCount++;
      historyList_->put(key, historyCount);

      // 如果数据在主缓存中，直接返回
      if (inMainCache)
      {
        return value;
      }

      // 如果数据不在主缓存，但访问次数达到了k次
      if (historyCount >= k_)
      {
        // 检查是否有历史值记录
        auto it = historyValueMap_.find(key);
        if (it != historyValueMap_.end())
        {
          // 有历史值，将其添加到主缓存
          Value storedValue = it->second;

          // 从历史记录移除
          historyList_->remove(key);
          historyValueMap_.erase(it);

          // 添加到主缓存
          RainLru<Key, Value>::put(key, storedValue);

          return storedValue;
        }
        // 没有历史值记录，无法添加到缓存
      }

      // 数据不在主缓存且不满足添加条件，返回默认值
      return value;
    }

    // 存入缓存
    void put(Key key, Value value)
    {
      // 检查是否已在主缓存
      Value existingValue{};
      bool inMainCache = RainLru<Key, Value>::get(key, existingValue);

      if (inMainCache)
      {
        // 已在主缓存，直接更新
        RainLru<Key, Value>::put(key, value);
        return;
      }

      // 获取并更新访问历史
      size_t historyCount = historyList_->get(key);
      historyCount++;
      historyList_->put(key, historyCount);

      // 保存值到历史记录映射，供后续get操作使用
      historyValueMap_[key] = value;

      // 检查是否达到k次访问阈值
      if (historyCount >= k_)
      {
        // 达到阈值，添加到主缓存
        historyList_->remove(key);
        historyValueMap_.erase(key);
        RainLru<Key, Value>::put(key, value);
      }
    }

  private:
    int k_;                                             // 进入缓存队列的评判标准
    std::unique_ptr<RainLru<Key, size_t>> historyList_; // 访问数据历史记录(value为访问次数)
    std::unordered_map<Key, Value> historyValueMap_;    // 存储未达到k次访问的数据值
  };

  // Lru 分片优化，提高并发性能
  template <typename Key, typename Value>
  class RainLruHash
  {
  public:
    // sliceNum 默认使用系统的硬件并发数 std::thread::hardware_concurrency()
    explicit RainLruHash(size_t capacity, int sliceNum)
        : capacity_(capacity),
          sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
    {
      size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // 获取每个分片的大小
      for (int i = 0; i < sliceNum_; ++i)
      {
        lruSliceCaches_.emplace_back(new RainLru<Key, Value>(sliceSize));
      }
    }

    // 每个分片都是一个 LRU
    void put(Key key, Value value)
    {
      // 获取key的hash值，并计算出对应的分片索引
      size_t sliceIndex = Hash(key) % sliceNum_;
      lruSliceCaches_[sliceIndex]->put(key, value);
    }

    // 查询接口 1
    bool get(Key key, Value &value)
    {
      // 获取key的hash值，并计算出对应的分片索引
      size_t sliceIndex = Hash(key) % sliceNum_;
      return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    // 查询接口 2
    Value get(Key key)
    {
      Value value;
      get(key, value);
      return value;
    }

  private:
    // 将key转换为对应hash值
    size_t Hash(Key key)
    {
      std::hash<Key> hashFunc;
      return hashFunc(key);
    }

  private:
    size_t capacity_;                                                  // 总容量
    int sliceNum_;                                                     // 切片数量
    std::vector<std::unique_ptr<RainLru<Key, Value>>> lruSliceCaches_; // 切片LRU缓存
  };
} // namespace RainCache