// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#ifndef _SMALLHASHTABLE_H_
#define _SMALLHASHTABLE_H_

//------------------------------------------------------------------------
// HashTableInfo: a concept that provides equality and hashing methods for
//                a particular key type. Used by HashTableBase and its
//                subclasses.
template<typename TKey>
struct HashTableInfo
{
    // static bool Equals(const TKey& x, const TKey& y);
    // static unsigned GetHashCode(const TKey& key);
};

//------------------------------------------------------------------------
// HashTableInfo<TKey*>: specialized version of HashTableInfo for pointer-
//                       typed keys.
template<typename TKey>
struct HashTableInfo<TKey*>
{
    static bool Equals(const TKey* x, const TKey* y)
    {
        return x == y;
    }

    static unsigned GetHashCode(const TKey* key)
    {
        // Shift off bits that are not likely to be significant
        size_t keyval = reinterpret_cast<size_t>(key) >> ConstLog2<__alignof(TKey)>::value;

        // Truncate and return the result
        return static_cast<unsigned>(keyval);
    }
};

//------------------------------------------------------------------------
// HashTableBase: base type for HashTable and SmallHashTable. This class
//                provides the vast majority of the implementation. The
//                subclasses differ in the storage they use at the time of
//                construction: HashTable allocates the initial bucket
//                array on the heap; SmallHashTable contains a small inline
//                array.
//
// This implementation is based on the ideas presented in Herlihy, Shavit,
// and Tzafrir '08 (http://mcg.cs.tau.ac.il/papers/disc2008-hopscotch.pdf),
// though it does not currently implement the hopscotch algorithm.
//
// The approach taken is intended to perform well in both space and speed.
// This approach is a hybrid of separate chaining and open addressing with
// linear probing: collisions are resolved using a bucket chain, but that
// chain is stored in the bucket array itself.
//
// Resolving collisions using a bucket chain avoids the primary clustering
// issue common in linearly-probed open addressed hash tables, while using
// buckets as chain nodes avoids the allocaiton traffic typical of chained
// tables. Applying the hopscotch algorithm in the aforementioned paper
// could further improve performance by optimizing access patterns for
// better cache usage.
//
// Template parameters:
//    TKey     - The type of the table's keys.
//    TValue   - The type of the table's values.
//    TKeyInfo - A type that conforms to the HashTableInfo<TKey> concept.
template<typename TKey, typename TValue, typename TKeyInfo = HashTableInfo<TKey>>
class HashTableBase
{
    friend class KeyValuePair;
    friend class Iterator;

    enum : unsigned
    {
        InitialNumBuckets = 8
    };

protected:
    //------------------------------------------------------------------------
    // HashTableBase::Bucket: provides storage for the key-value pairs that
    //                        make up the contents of the table.
    //
    // The "home" bucket for a particular key is the bucket indexed by the
    // key's hash code modulo the size of the bucket array (the "home index").
    //
    // The home bucket is always considered to be part of the chain that it
    // roots, even if it is also part of the chain rooted at a different
    // bucket. `m_firstOffset` indicates the offset of the first non-home
    // bucket in the home bucket's chain. If the `m_firstOffset` of a bucket
    // is 0, the chain rooted at that bucket is empty.
    //
    // The index of the next bucket in a chain is calculated by adding the
    // value in `m_nextOffset` to the index of the current bucket. If
    // `m_nextOffset` is 0, the current bucket is the end of its chain. Each
    // bucket in a chain must be occupied (i.e. `m_isFull` will be true).
    struct Bucket
    {
        bool m_isFull;          // True if the bucket is occupied; false otherwise.

        unsigned m_firstOffset; // The offset to the first node in the chain for this bucket index.
        unsigned m_nextOffset;  // The offset to the next node in the chain for this bucket index.

        unsigned m_hash;        // The hash code for the element stored in this bucket.
        TKey m_key;             // The key for the element stored in this bucket.
        TValue m_value;         // The value for the element stored in this bucket.
    };

private:
    Compiler* m_compiler;      // The compiler context to use for allocations.
    Bucket* m_buckets;         // The bucket array.
    unsigned m_numBuckets;     // The number of buckets in the bucket array.
    unsigned m_numFullBuckets; // The number of occupied buckets.

    //------------------------------------------------------------------------
    // HashTableBase::Insert: inserts a key-value pair into a bucket array.
    //
    // Arguments:
    //    buckets    - The bucket array in which to insert the key-value pair.
    //    numBuckets - The number of buckets in the bucket array.
    //    hash       - The hash code of the key to insert.
    //    key        - The key to insert.
    //    value      - The value to insert.
    //
    // Returns:
    //    True if the key-value pair was successfully inserted; false
    //    otherwise.
    static bool Insert(Bucket* buckets, unsigned numBuckets, unsigned hash, const TKey& key, const TValue& value)
    {
        const unsigned mask = numBuckets - 1;
        unsigned homeIndex = hash & mask;

        Bucket* home = &buckets[homeIndex];
        if (!home->m_isFull)
        {
            // The home bucket is empty; use it.
            //
            // Note that the next offset does not need to be updated: whether or not it is non-zero,
            // it is already correct, since we're inserting at the head of the list.
            home->m_isFull = true;
            home->m_firstOffset = 0;
            home->m_hash = hash;
            home->m_key = key;
            home->m_value = value;
            return true;
        }

        // If the home bucket is full, probe to find the next empty bucket.
        unsigned precedingIndexInChain = homeIndex;
        unsigned nextIndexInChain = (homeIndex + home->m_firstOffset) & mask;
        for (unsigned j = 1; j < numBuckets; j++)
        {
            unsigned bucketIndex = (homeIndex + j) & mask;
            Bucket* bucket = &buckets[bucketIndex];
            if (bucketIndex == nextIndexInChain)
            {
                assert(bucket->m_isFull);
                precedingIndexInChain = bucketIndex;
                nextIndexInChain = (bucketIndex + bucket->m_nextOffset) & mask;
            }
            else if (!bucket->m_isFull)
            {
                bucket->m_isFull = true;
                if (precedingIndexInChain == nextIndexInChain)
                {
                    bucket->m_nextOffset = 0;
                }
                else
                {
                    assert(((nextIndexInChain - bucketIndex) & mask) > 0);
                    bucket->m_nextOffset = (nextIndexInChain - bucketIndex) & mask;
                }

                unsigned offset = (bucketIndex - precedingIndexInChain) & mask;
                if (precedingIndexInChain == homeIndex)
                {
                    buckets[precedingIndexInChain].m_firstOffset = offset;
                }
                else
                {
                    buckets[precedingIndexInChain].m_nextOffset = offset;
                }

                bucket->m_hash = hash;
                bucket->m_key = key;
                bucket->m_value = value;
                return true;
            }
        }

        // No more free buckets.
        return false;
    }

    //------------------------------------------------------------------------
    // HashTableBase::TryGetBucket: attempts to get the bucket that holds a
    //                              particular key.
    //
    // Arguments:
    //    hash           - The hash code of the key to find.
    //    key            - The key to find.
    //    precedingIndex - An output parameter that will hold the index of the
    //                     preceding bucket in the chain for the key. May be
    //                     equal to `bucketIndex` if the key is stored in its
    //                     home bucket.
    //    bucketIndex    - An output parameter that will hold the index of the
    //                     bucket that stores the key.
    //
    // Returns:
    //    True if the key was successfully found; false otherwise.
    bool TryGetBucket(unsigned hash, const TKey& key, unsigned* precedingIndex, unsigned* bucketIndex) const
    {
        if (m_numBuckets == 0)
        {
            return false;
        }

        const unsigned mask = m_numBuckets - 1;
        unsigned index = hash & mask;

        Bucket* bucket = &m_buckets[index];
        if (bucket->m_isFull && bucket->m_hash == hash && TKeyInfo::Equals(bucket->m_key, key))
        {
            *precedingIndex = index;
            *bucketIndex = index;
            return true;
        }

        for (unsigned offset = bucket->m_firstOffset; offset != 0; offset = bucket->m_nextOffset)
        {
            unsigned precedingIndexInChain = index;

            index = (index + offset) & mask;
            bucket = &m_buckets[index];

            assert(bucket->m_isFull);
            if (bucket->m_hash == hash && TKeyInfo::Equals(bucket->m_key, key))
            {
                *precedingIndex = precedingIndexInChain;
                *bucketIndex = index;
                return true;
            }
        }

        return false;
    }

    //------------------------------------------------------------------------
    // HashTableBase::Resize: allocates a new bucket array twice the size of
    //                        the current array and copies the key-value pairs
    //                        from the current bucket array into the new array.
    void Resize()
    {
        Bucket* currentBuckets = m_buckets;

        unsigned newNumBuckets = m_numBuckets == 0 ? InitialNumBuckets : m_numBuckets * 2;
        size_t allocSize = sizeof(Bucket) * newNumBuckets;
        assert((sizeof(Bucket) * m_numBuckets) < allocSize);

        auto* newBuckets = reinterpret_cast<Bucket*>(m_compiler->compGetMem(allocSize));
        memset(newBuckets, 0, allocSize);

        for (unsigned currentIndex = 0; currentIndex < m_numBuckets; currentIndex++)
        {
            Bucket* currentBucket = &currentBuckets[currentIndex];
            if (!currentBucket->m_isFull)
            {
                continue;
            }

            bool inserted = Insert(newBuckets, newNumBuckets, currentBucket->m_hash, currentBucket->m_key, currentBucket->m_value);
            (assert(inserted), (void)inserted);
        }

        m_numBuckets = newNumBuckets;
        m_buckets = newBuckets;
    }

protected:
    HashTableBase(Compiler* compiler, Bucket* buckets, unsigned numBuckets)
        : m_compiler(compiler)
        , m_buckets(buckets)
        , m_numBuckets(numBuckets)
        , m_numFullBuckets(0)
    {
        assert(compiler != nullptr);

        if (numBuckets > 0)
        {
            assert((numBuckets & (numBuckets - 1)) == 0); // Size must be a power of 2
            assert(m_buckets != nullptr);

            memset(m_buckets, 0, sizeof(Bucket) * numBuckets);
        }
    }

public:
#ifdef DEBUG
    class Iterator;

    class KeyValuePair final
    {
        friend class HashTableBase<TKey, TValue, TKeyInfo>::Iterator;

        Bucket* m_bucket;

        KeyValuePair(Bucket* bucket)
            : m_bucket(bucket)
        {
            assert(m_bucket != nullptr);
        }

    public:
        KeyValuePair()
            : m_bucket(nullptr)
        {
        }

        inline TKey& Key()
        {
            return m_bucket->m_key;
        }

        inline TValue& Value()
        {
            return m_bucket->m_value;
        }
    };

    // NOTE: HashTableBase only provides iterators in debug builds because the order in which
    // the iterator type produces values is undefined (e.g. it is not related to the order in
    // which key-value pairs were inserted).
    class Iterator final
    {
        friend class HashTableBase<TKey, TValue, TKeyInfo>;

        Bucket* m_buckets;
        unsigned m_numBuckets;
        unsigned m_index;

        Iterator(Bucket* buckets, unsigned numBuckets, unsigned index)
            : m_buckets(buckets)
            , m_numBuckets(numBuckets)
            , m_index(index)
        {
            assert((buckets != nullptr) || (numBuckets == 0));
            assert(index <= numBuckets);

            // Advance to the first occupied bucket
            while (m_index != m_numBuckets && !m_buckets[m_index].m_isFull)
            {
                m_index++;
            }
        }

    public:
        Iterator()
            : m_buckets(nullptr)
            , m_numBuckets(0)
            , m_index(0)
        {
        }

        KeyValuePair operator*() const
        {
            if (m_index >= m_numBuckets)
            {
                return KeyValuePair();
            }

            Bucket* bucket = &m_buckets[m_index];
            assert(bucket->m_isFull);
            return KeyValuePair(bucket);
        }

        KeyValuePair operator->() const
        {
            return this->operator*();
        }

        bool operator==(const Iterator& other) const
        {
            return (m_buckets == other.m_buckets) && (m_index == other.m_index);
        }

        bool operator!=(const Iterator& other) const
        {
            return (m_buckets != other.m_buckets) || (m_index != other.m_index);
        }

        Iterator& operator++()
        {
            do
            {
                m_index++;
            } while (m_index != m_numBuckets && !m_buckets[m_index].m_isFull);

            return *this;
        }
    };

    Iterator begin() const
    {
        return Iterator(m_buckets, m_numBuckets, 0);
    }

    Iterator end() const
    {
        return Iterator(m_buckets, m_numBuckets, m_numBuckets);
    }
#endif // DEBUG

    unsigned Count() const
    {
        return m_numFullBuckets;
    }

    void Clear()
    {
        if (m_numBuckets > 0)
        {
            memset(m_buckets, 0, sizeof(Bucket) * m_numBuckets);
            m_numFullBuckets = 0;
        }
    }

    //------------------------------------------------------------------------
    // HashTableBase::AddOrUpdate: adds a key-value pair to the hash table if
    //                             the key does not already exist in the
    //                             table, or updates the value if the key
    //                             already exists.
    //                             
    // Arguments:
    //    key   - The key for which to add or update a value.
    //    value - The value.
    //
    // Returns:
    //    True if the value was added; false if it was updated.
    bool AddOrUpdate(const TKey& key, const TValue& value)
    {
        unsigned hash = TKeyInfo::GetHashCode(key);

        unsigned unused, index;
        if (TryGetBucket(hash, key, &unused, &index))
        {
            m_buckets[index].m_value = value;
            return false;
        }

        // If the load is greater than 0.8, resize the table before inserting.
        if ((m_numFullBuckets * 5) >= (m_numBuckets * 4))
        {
            Resize();
        }

        bool inserted = Insert(m_buckets, m_numBuckets, hash, key, value);
        (assert(inserted), (void)inserted);

        m_numFullBuckets++;

        return true;
    }

    //------------------------------------------------------------------------
    // HashTableBase::TryRemove: removes a key from the hash table and returns
    //                           its value if the key exists in the table.
    //
    // Arguments:
    //    key   - The key to remove from the table.
    //    value - An output parameter that will hold the value for the removed
    //            key.
    //
    // Returns:
    //    True if the key was removed from the table; false otherwise.
    bool TryRemove(const TKey& key, TValue* value)
    {
        unsigned hash = TKeyInfo::GetHashCode(key);

        unsigned precedingIndexInChain, bucketIndex;
        if (!TryGetBucket(hash, key, &precedingIndexInChain, &bucketIndex))
        {
            return false;
        }

        Bucket* bucket = &m_buckets[bucketIndex];
        bucket->m_isFull = false;

        if (precedingIndexInChain != bucketIndex)
        {
            const unsigned mask = m_numBuckets - 1;
            unsigned homeIndex = hash & mask;

            unsigned nextOffset;
            if (bucket->m_nextOffset == 0)
            {
                nextOffset = 0;
            }
            else
            {
                unsigned nextIndexInChain = (bucketIndex + bucket->m_nextOffset) & mask;
                nextOffset = (nextIndexInChain - precedingIndexInChain) & mask;
            }

            if (precedingIndexInChain == homeIndex)
            {
                m_buckets[precedingIndexInChain].m_firstOffset = nextOffset;
            }
            else
            {
                m_buckets[precedingIndexInChain].m_nextOffset = nextOffset;
            }
        }

        m_numFullBuckets--;

        *value = bucket->m_value;
        return true;
    }

    //------------------------------------------------------------------------
    // HashTableBase::TryGetValue: retrieves the value for a key if the key
    //                             exists in the table.
    //
    // Arguments:
    //    key   - The key to find from the table.
    //    value - An output parameter that will hold the value for the key.
    //
    // Returns:
    //    True if the key was found in the table; false otherwise.
    bool TryGetValue(const TKey& key, TValue* value) const
    {
        unsigned unused, index;
        if (!TryGetBucket(TKeyInfo::GetHashCode(key), key, &unused, &index))
        {
            return false;
        }

        *value = m_buckets[index].m_value;
        return true;
    }
};

//------------------------------------------------------------------------
// HashTable: a simple subclass of `HashTableBase` that always uses heap
//            storage for its bucket array.
template<typename TKey, typename TValue, typename TKeyInfo = HashTableInfo<TKey>>
class HashTable final : public HashTableBase<TKey, TValue, TKeyInfo>
{
    typedef HashTableBase<TKey, TValue, TKeyInfo> TBase;

    static unsigned RoundUp(unsigned initialSize)
    {
        return 1 << genLog2(initialSize);
    }

public:
    HashTable(Compiler* compiler)
        : TBase(compiler, nullptr, 0)
    {
    }

    HashTable(Compiler* compiler, unsigned initialSize)
        : TBase(compiler,
            reinterpret_cast<typename TBase::Bucket*>(compiler->compGetMem(RoundUp(initialSize) * sizeof(typename TBase::Bucket))),
            RoundUp(initialSize))
    {
    }
};

//------------------------------------------------------------------------
// SmallHashTable: an alternative to `HashTable` that stores the initial
//                 bucket array inline. Most useful for situations where
//                 the number of key-value pairs that will be stored in
//                 the map at any given time falls below a certain
//                 threshold. Switches to heap storage once the initial
//                 inline storage is exhausted.
template<typename TKey, typename TValue, unsigned NumInlineBuckets = 8, typename TKeyInfo = HashTableInfo<TKey>>
class SmallHashTable final : public HashTableBase<TKey, TValue, TKeyInfo>
{
    typedef HashTableBase<TKey, TValue, TKeyInfo> TBase;

    enum : unsigned
    {
        RoundedNumInlineBuckets = 1 << ConstLog2<NumInlineBuckets>::value
    };

    typename TBase::Bucket m_inlineBuckets[RoundedNumInlineBuckets];

public:
    SmallHashTable(Compiler* compiler)
        : TBase(compiler, m_inlineBuckets, RoundedNumInlineBuckets)
    {
    }
};

#endif // _SMALLHASHTABLE_H_
