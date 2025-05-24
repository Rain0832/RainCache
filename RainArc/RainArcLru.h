#pragma once

#include "RainArcNode.h"
#include <unordered_map>
#include <mutex>

namespace RainCache
{
  template <typename Key, typename Value>
  class ArcLruPart
  {
  public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    // 构造函数
    explicit ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity),
          ghostCapacity_(capacity),
          transformThreshold_(transformThreshold)
    {
      initializeLists();
    }

    // 存入缓存
    bool put(Key key, Value value)
    {
      if (capacity_ == 0)
        return false;

      std::lock_guard<std::mutex> lock(mutex_);
      auto it = mainCache_.find(key);
      if (it != mainCache_.end())
      {
        return updateExistingNode(it->second, value);
      }
      return addNewNode(key, value);
    }

    // 查询缓存，参数：是否需要移动
    bool get(Key key, Value &value, bool &shouldTransform)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = mainCache_.find(key);
      if (it != mainCache_.end())
      {
        shouldTransform = updateNodeAccess(it->second);
        value = it->second->getValue();
        return true;
      }
      return false;
    }

    // 查询幽灵
    bool checkGhost(Key key)
    {
      auto it = ghostCache_.find(key);
      if (it != ghostCache_.end())
      {
        removeFromGhost(it->second);
        ghostCache_.erase(it);
        return true;
      }
      return false;
    }

    // 增加 Lru 容量
    void increaseCapacity()
    {
      ++capacity_;
    }

    // 减小 Lru 容量
    bool decreaseCapacity()
    {
      if (capacity_ <= 0)
        return false;
      if (mainCache_.size() == capacity_)
      {
        evictLeastRecent();
      }
      --capacity_;
      return true;
    }

  private:
    // 初始化 Lru 链表
    void initializeLists()
    {
      mainHead_ = std::make_shared<NodeType>();
      mainTail_ = std::make_shared<NodeType>();
      mainHead_->next_ = mainTail_;
      mainTail_->prev_ = mainHead_;

      ghostHead_ = std::make_shared<NodeType>();
      ghostTail_ = std::make_shared<NodeType>();
      ghostHead_->next_ = ghostTail_;
      ghostTail_->prev_ = ghostHead_;
    }

    // 存在缓存里的节点，更新数值
    bool updateExistingNode(NodePtr node, const Value &value)
    {
      node->setValue(value);
      moveToFront(node);
      return true;
    }

    // 添加新的节点
    bool addNewNode(const Key &key, const Value &value)
    {
      if (mainCache_.size() >= capacity_)
      {
        evictLeastRecent(); // 驱逐最近最少访问
      }

      NodePtr newNode = std::make_shared<NodeType>(key, value);
      mainCache_[key] = newNode;
      addToFront(newNode);
      return true;
    }

    // 存在缓存里的节点，更新访问
    bool updateNodeAccess(NodePtr node)
    {
      moveToFront(node);
      node->incrementAccessCount();
      return node->getAccessCount() >= transformThreshold_;
    }

    // 移动至头部
    void moveToFront(NodePtr node)
    {
      // 先从当前位置移除
      if (!node->prev_.expired() && node->next_)
      {
        auto prev = node->prev_.lock();
        prev->next_ = node->next_;
        node->next_->prev_ = node->prev_;
        node->next_ = nullptr; // 清空指针，防止悬垂引用
      }

      // 添加到头部
      addToFront(node);
    }

    // 添加至头部
    void addToFront(NodePtr node)
    {
      node->next_ = mainHead_->next_;
      node->prev_ = mainHead_;
      mainHead_->next_->prev_ = node;
      mainHead_->next_ = node;
    }

    // 移除尾部
    void evictLeastRecent()
    {
      NodePtr leastRecent = mainTail_->prev_.lock();
      if (!leastRecent || leastRecent == mainHead_)
        return;

      // 从主链表中移除
      removeFromMain(leastRecent);

      // 添加到幽灵缓存
      if (ghostCache_.size() >= ghostCapacity_)
      {
        removeOldestGhost();
      }
      addToGhost(leastRecent);

      // 从主缓存映射中移除
      mainCache_.erase(leastRecent->getKey());
    }

    // 从主缓存链表中删除
    void removeFromMain(NodePtr node)
    {
      if (!node->prev_.expired() && node->next_)
      {
        auto prev = node->prev_.lock();
        prev->next_ = node->next_;
        node->next_->prev_ = node->prev_;
        node->next_ = nullptr; // 清空指针，防止悬垂引用
      }
    }

    // 从幽灵链表中删除
    void removeFromGhost(NodePtr node)
    {
      if (!node->prev_.expired() && node->next_)
      {
        auto prev = node->prev_.lock();
        prev->next_ = node->next_;
        node->next_->prev_ = node->prev_;
        node->next_ = nullptr; // 清空指针，防止悬垂引用
      }
    }

    // 添加到幽灵链表中
    void addToGhost(NodePtr node)
    {
      // 重置节点的访问计数
      node->accessCount_ = 1;

      // 添加到幽灵缓存的头部
      node->next_ = ghostHead_->next_;
      node->prev_ = ghostHead_;
      ghostHead_->next_->prev_ = node;
      ghostHead_->next_ = node;

      // 添加到幽灵缓存映射
      ghostCache_[node->getKey()] = node;
    }

    // 移除幽灵链表尾部
    void removeOldestGhost()
    {
      // 使用lock()方法，并添加null检查
      NodePtr oldestGhost = ghostTail_->prev_.lock();
      if (!oldestGhost || oldestGhost == ghostHead_)
        return;

      removeFromGhost(oldestGhost);
      ghostCache_.erase(oldestGhost->getKey());
    }

  private:
    size_t capacity_;
    size_t ghostCapacity_;
    size_t transformThreshold_; // 转换门槛值
    std::mutex mutex_;

    NodeMap mainCache_; // key -> ArcNode
    NodeMap ghostCache_;

    // 主链表
    NodePtr mainHead_;
    NodePtr mainTail_;
    // 淘汰链表
    NodePtr ghostHead_;
    NodePtr ghostTail_;
  };
}