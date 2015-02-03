#pragma once

#include <MathTypes.h>
#include <vector>
#include "e_ErrorHandler.h"
#include "..\Base\FileStream.h"

template<typename T> struct e_KernelBuffer
{
	T* Data;
	unsigned int UsedCount;
	unsigned int Length;
	CUDA_FUNC_IN T& operator[](unsigned int i) const
	{
		return Data[i];
	}
};

template<typename H, typename D> class e_BufferReference;
template<typename H, typename D> class e_Buffer
{
	friend e_BufferReference<H, D>;
protected:
	H* host;
	D* device;
	D* deviceMapped;
	unsigned int m_uPos;
	unsigned int m_uLength;
	unsigned int m_uHostBlockSize;
	typedef std::pair<unsigned int, unsigned int> part;
	std::vector<part> m_sInvalidated;
	std::vector<part> m_sDeallocated;
public:
	e_Buffer(unsigned int a_NumElements, unsigned int a_ElementSize = 0xffffffff)
	{
		if(a_ElementSize == 0xffffffff)
			a_ElementSize = sizeof(H);
		m_uHostBlockSize = a_ElementSize;
		m_uPos = 0;
		m_uLength = a_NumElements;
		host = (H*)::malloc(m_uHostBlockSize * a_NumElements);
		Platform::SetMemory(host, m_uHostBlockSize * a_NumElements);
		CUDA_MALLOC(&device, sizeof(D) * a_NumElements);
		cudaMemset(device, 0, sizeof(D) * a_NumElements);
		deviceMapped = (D*)::malloc(a_NumElements * sizeof(D));
		memset(deviceMapped, 0, sizeof(D) * a_NumElements);
	}
	void Free()
	{
		free(host);
		if(deviceMapped)
			free(deviceMapped);
		CUDA_FREE(device);
	}
	e_Buffer(InputStream& a_In)
	{
		a_In >> m_uLength;
		a_In >> m_uPos;
		a_In >> m_uHostBlockSize;
		unsigned int deN;
		a_In >> deN;
		if(CUDA_MALLOC(&device, sizeof(D) * m_uLength))
			BAD_CUDA_ALLOC(sizeof(D) * m_uLength)
		host = (H*)::malloc(m_uHostBlockSize * m_uLength);
		a_In.Read(host, m_uPos * m_uHostBlockSize);
		m_sDeallocated.resize(deN);
		if(deN)
			a_In.Read(&m_sDeallocated[0], deN * sizeof(part));
		Invalidate();
	}
	void Serialize(OutputStream& a_Out)
	{
		a_Out << m_uLength;
		a_Out << m_uPos;
		a_Out << m_uHostBlockSize;
		a_Out << (unsigned int)m_sDeallocated.size();
		a_Out.Write(host, m_uPos * m_uHostBlockSize);
		if(m_sDeallocated.size())
			a_Out.Write(&m_sDeallocated[0], m_sDeallocated.size() * sizeof(part));
	}
	e_BufferReference<H, D> malloc(unsigned int a_Count)
	{
		for(unsigned int i = 0; i < m_sDeallocated.size(); i++)
			if(m_sDeallocated[i].second >= a_Count)
			{
				part& p = m_sDeallocated[i];
				if(p.second == a_Count)
				{
					m_sDeallocated.erase(m_sDeallocated.begin() + i);
					return e_BufferReference<H, D>(this, p.first, a_Count);
				}
				else
				{
					p.second = p.second - a_Count;
					unsigned int s = p.first;
					p.first += a_Count;
					return e_BufferReference<H, D>(this, s, a_Count);
				}
			}
		if(a_Count <= m_uLength - m_uPos)
		{
			m_uPos += a_Count;//RoundUp(a_Count, 16)
			return e_BufferReference<H, D>(this, m_uPos - a_Count, a_Count);
		}
		else
		{
			//BAD_EXCEPTION("Cuda data stream malloc failure, %d elements requested, %d available.", a_Count, m_uLength - m_uPos)
			//return e_BufferReference<H, D>();
			unsigned int newLength = m_uPos + a_Count;
			cudaFree(device);
			free(deviceMapped);
			H* newHost = (H*)::malloc(m_uHostBlockSize * newLength);
			::memcpy(newHost, host, m_uPos * m_uHostBlockSize);
			free(host);
			host = newHost;
			m_uLength = newLength;
			CUDA_MALLOC(&device, sizeof(D) * newLength);
			cudaMemset(device, 0, sizeof(D) * newLength);
			deviceMapped = (D*)::malloc(newLength * sizeof(D));
			memset(deviceMapped, 0, sizeof(D) * newLength);
			Invalidate(0, m_uPos);
			return malloc(a_Count);
		}
	}
	e_BufferReference<H, D> malloc(e_BufferReference<H, D> r, bool copyToNew = true)
	{
		e_BufferReference<H, D> r2 = malloc(r.getLength());
		if(copyToNew)
			memcpy(r, r2);
		return r2;
	}
	///Copies dest->length bytes from source to dest, on the host aswell as the device
	void memcpy(e_BufferReference<H, D> source, e_BufferReference<H, D> dest)
	{
		if(source.getLength() < dest.getLength() || source.m_pStream != this || dest.m_pStream != this)
			throw 1;
		::memcpy(dest.atH(dest->getIndex()), source.atH(source.getIndex()), m_uHostBlockSize * dest.getLength());
		Invalidate(dest);
	}
	void dealloc(e_BufferReference<H, D> a_Ref)
	{
		if(a_Ref.getIndex() + a_Ref.getLength() == m_uPos)
			m_uPos -= a_Ref.getLength();
		else
		{
			for(unsigned int i = 0; i < m_sDeallocated.size(); i++)
				if(m_sDeallocated[i].first == a_Ref.getIndex() + a_Ref.getLength())
				{
					m_sDeallocated[i].first = a_Ref.getIndex();
					m_sDeallocated[i].second += a_Ref.getLength();
					return;
				}
			m_sDeallocated.push_back(std::make_pair(a_Ref.getIndex(), a_Ref.getLength()));
		}
	}
	void dealloc(unsigned int i, unsigned int j = 1)
	{
		dealloc(operator()(i, j));
	}
	void Invalidate()
	{
		m_sInvalidated.clear();
		m_sInvalidated.push_back(std::make_pair(0U, numElements));
	}
	void Invalidate(e_BufferReference<H, D> a_Ref)
	{	
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			if(m_sInvalidated[i].first <= a_Ref.getIndex() && m_sInvalidated[i].second + m_sInvalidated[i].first >= a_Ref.getIndex() + a_Ref.getLength())
				return;
			else if(m_sInvalidated[i].first <= a_Ref.getIndex() && m_sInvalidated[i].first + m_sInvalidated[i].second >= a_Ref.getIndex())
			{
				m_sInvalidated[i].second = a_Ref.getIndex() + a_Ref.getLength() - m_sInvalidated[i].first;
				return;
			}
		}
		m_sInvalidated.push_back(std::make_pair(a_Ref.getIndex(), a_Ref.getLength()));
	}
	void Invalidate(unsigned int i, unsigned int j = 1)
	{
		Invalidate(operator()(i, j));
	}
	void UpdateInvalidated()
	{
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			for(unsigned int j = 0; j < m_sInvalidated[i].second; j++)
			{
				unsigned int k = j + m_sInvalidated[i].first;
				deviceMapped[k] = operator()(k)->getKernelData();
			}
		}
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			int q = sizeof(D), f = m_sInvalidated[i].first, n = q * m_sInvalidated[i].second;
			cudaMemcpy(device + f, deviceMapped + f, n, cudaMemcpyHostToDevice);
		}
		m_sInvalidated.clear();
	}
	template<typename CBT> void UpdateInvalidatedCB(CBT& f)
	{
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			for(unsigned int j = 0; j < m_sInvalidated[i].second; j++)
			{
				unsigned int k = j + m_sInvalidated[i].first;
				f(operator()(k));//do that BEFORE, so the user has a chance to modify the data which will be copied...
				deviceMapped[k] = operator()(k)->getKernelData();
			}
		}
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			int q = sizeof(D), f = m_sInvalidated[i].first, n = q * m_sInvalidated[i].second;
			cudaMemcpy(device + f, deviceMapped + f, n, cudaMemcpyHostToDevice);
		}
		m_sInvalidated.clear();
	}
	unsigned int getLength() const
	{
		return m_uLength;
	}
	e_BufferReference<H, D> operator()(unsigned int i = 0, unsigned int l = 1)
	{
		if (i >= m_uLength)
			BAD_EXCEPTION("Invalid index of %d, length is %d", i, m_uLength)
		return e_BufferReference<H, D>(this, i, l);
	}
	unsigned int NumUsedElements() const
	{
		return m_uPos;
	}
	e_BufferReference<H, D> UsedElements()
	{
		return e_BufferReference<H, D>(this, 0, m_uLength - m_uPos);
	}
	unsigned int getSizeInBytes() const
	{
		return m_uLength * sizeof(D);
	}
	e_KernelBuffer<D> getKernelData(bool devicePointer = true) const
	{
		e_KernelBuffer<D> r;
		r.Data = devicePointer ? device : deviceMapped;
		r.Length = m_uLength;
		r.UsedCount = m_uPos;
		return r;
	}
	e_BufferReference<H, D> translate(const H* val) const
	{
		unsigned long long t0 = ((unsigned long long)val - (unsigned long long)host) / sizeof(H), t1 = ((unsigned long long)val - (unsigned long long)device) / sizeof(D);
		unsigned int i = t0 < m_uLength ? t0 : t1;
		return e_BufferReference<H, D>(this, i, 1);
	}
	e_BufferReference<H, D> translate(const D* val) const
	{
		unsigned long long t0 = ((unsigned long long)val - (unsigned long long)host) / sizeof(H), t1 = ((unsigned long long)val - (unsigned long long)device) / sizeof(D);
		unsigned int i = t0 < m_uLength ? t0 : t1;
		return e_BufferReference<H, D>(this, i, 1);
	}
	D* getDeviceMapped(int i) const
	{
		return deviceMapped + i;
	}
};

template<typename H, typename D> class e_BufferReference
{
public:
	unsigned int m_uIndex;
	unsigned int m_uLength;
	e_Buffer<H, D>* m_pStream;
public:
	e_BufferReference()
	{
		m_uIndex = m_uLength = 0;
		m_pStream = 0;
	}
	e_BufferReference(e_Buffer<H, D>* s, unsigned int i, unsigned int c)
	{
		m_uIndex = i;
		m_uLength = c;
		m_pStream = s;
	}
	unsigned int getIndex() const
	{
		return m_uIndex;
	}
	unsigned int getLength() const
	{
		return m_uLength;
	}
	unsigned int getSizeInBytes() const
	{
		return m_uLength * sizeof(H);
	}
	void Invalidate()
	{
		m_pStream->Invalidate(*this);
	}
	e_BufferReference<H, D> operator()(unsigned int i = 0) const
	{
		return e_BufferReference<H, D>(m_pStream, m_uIndex + i, 1);
	}
	operator bool() const
	{
		return m_uLength;
	}
	e_BufferReference<H, D>& operator= (const H &r)
	{
		*atH(m_uIndex) = r;
		return *this;
	}
	H* operator->() const
	{
		return atH(m_uIndex);
	}
	D* getDeviceMapped() const
	{
		return m_pStream->getDeviceMapped(m_uIndex);
	}
	const H& operator*() const
	{
		return *atH(m_uIndex);
	}
	operator H*() const
	{
		return atH(m_uIndex);
	}
	D* getDevice() const
	{
		return atD(m_uIndex);
	}
	template<typename U> U* operator()(unsigned int i = 0) const
	{
		U* base = (U*)atH(m_uIndex);
		return base + i;
	}
	void ReadFrom(IInStream& a_In)
	{
		a_In.Read(operator H *(), getSizeInBytes());
		Invalidate();
	}
	void WriteTo(OutputStream& a_Out)
	{
		a_Out.Write(operator H *(), getSizeInBytes());
	}
	e_Variable<D> AsVar()
	{
		return AsVar<D>();
	}
	template<typename T> e_Variable<T> AsVar()
	{
		return e_Variable<T>((T*)getDeviceMapped(), (T*)getDevice());
	}
//private:
	H* atH(unsigned int i) const 
	{
		return (H*)((char*)m_pStream->host + m_pStream->m_uHostBlockSize * i);
	}
	D* atD(unsigned int i) const
	{
		return m_pStream->device + i;
	}
};

template<typename H, typename D> class e_CachedBuffer : public e_Buffer<H, D>
{
	inline bool strcmpare(const char* a, const char* b) const
	{
		size_t al = strlen(a), bl = strlen(b);
		if(al != bl)
			return false;
		else for(size_t i = 0; i < al; i++)
			if(a[i] != b[i])
				return false;
		return true;
	}
private:
	struct entry
	{
		unsigned int count;
		e_String file;
		entry()
		{
			count = 0;
			Platform::SetMemory(file, sizeof(file));
		}
	};
	std::vector<entry> m_sEntries;
public:
	e_CachedBuffer(unsigned int a_Count, unsigned int a_ElementSize = 0xffffffff)
		: e_Buffer<H, D>(a_Count, a_ElementSize)
	{
		
	}
	e_BufferReference<H, D> LoadCached(const char* file, bool* load)
	{
		for(unsigned int i = 0; i < m_sEntries.size(); i++)
			if(strcmpare(m_sEntries[i].file, file))
			{
				m_sEntries[i].count++;
				*load = false;
				return e_BufferReference<H, D>(this, i, 1);
			}
		*load = true;
		entry e;
		e.count = 1;
		Platform::SetMemory(e.file, sizeof(e.file));
		::memcpy(e.file, file, strlen(file));
		e_BufferReference<H, D> ref = malloc(1);
		if(m_sEntries.size() <= ref.getIndex())
			m_sEntries.push_back(e);
		else m_sEntries[ref.getIndex()] = e;
		return ref;
	}
	///Returns true noting free
	bool Release(e_BufferReference<H, D> ref)
	{
		unsigned int i = ref.getIndex();
		m_sEntries[i].count--;
		if(!m_sEntries[i].count)
		{
			this->dealloc(i);
			//m_sData.erase(i, i);//do not erase
			return true;
		}
		return false;
	}
};

#define e_StreamReference(T) e_BufferReference<T, T>
template<typename T> class e_Buffer<T, T>
{
	friend e_BufferReference<T, T>;
protected:
	T* host;
	T* device;
	T* deviceMapped;
	unsigned int m_uPos;
	unsigned int m_uLength;
	unsigned int m_uHostBlockSize;
	typedef std::pair<unsigned int, unsigned int> part;
	std::vector<part> m_sInvalidated;
	std::vector<part> m_sDeallocated;
public:
	e_Buffer(unsigned int N, unsigned int a_ElementSize = 0xffffffff)
	{
		a_ElementSize = sizeof(T);
		m_uHostBlockSize = a_ElementSize;
		m_uPos = 0;
		m_uLength = N;
		host = (T*)::malloc(m_uHostBlockSize * N);
		Platform::SetMemory(host, m_uHostBlockSize * N);
		CUDA_MALLOC(&device, sizeof(T) * N);
		cudaMemset(device, 0, sizeof(T) * N);
		deviceMapped = 0;
	}
	void Free()
	{
		free(host);
		if(deviceMapped)
			free(deviceMapped);
		CUDA_FREE(device);
	}
	e_Buffer(InputStream& a_In)
	{
		a_In >> m_uLength;
		a_In >> m_uPos;
		a_In >> m_uHostBlockSize;
		unsigned int deN;
		a_In >> deN;
		if(CUDA_MALLOC(&device, sizeof(T) * m_uLength))
			BAD_CUDA_ALLOC(sizeof(T) * m_uLength)
		host = (T*)::malloc(m_uHostBlockSize * m_uLength);
		a_In.Read(host, m_uPos * m_uHostBlockSize);
		m_sDeallocated.resize(deN);
		if(deN)
			a_In.Read(&m_sDeallocated[0], deN * sizeof(part));
		Invalidate();
	}
	void Serialize(OutputStream& a_Out)
	{
		a_Out << m_uLength;
		a_Out << m_uPos;
		a_Out << m_uHostBlockSize;
		a_Out << (unsigned int)m_sDeallocated.size();
		a_Out.Write(host, m_uPos * m_uHostBlockSize);
		if(m_sDeallocated.size())
			a_Out.Write(&m_sDeallocated[0], (int)m_sDeallocated.size() * sizeof(part));
	}
	e_BufferReference<T, T> malloc(unsigned int a_Count)
	{
		e_BufferReference<T, T> r;
		for(unsigned int i = 0; i < m_sDeallocated.size(); i++)
			if(m_sDeallocated[i].second >= a_Count)
			{
				part& p = m_sDeallocated[i];
				if(p.second == a_Count)
				{
					m_sDeallocated.erase(m_sDeallocated.begin() + i);
					r = e_BufferReference<T, T>(this, p.first, a_Count);
				}
				else
				{
					p.second = p.second - a_Count;
					unsigned int s = p.first;
					p.first += a_Count;
					r = e_BufferReference<T, T>(this, s, a_Count);
				}
				break;
			}
		if(a_Count <= m_uLength - m_uPos)
		{
			m_uPos += a_Count;//RoundUp(a_Count, 16)
			r = e_BufferReference<T, T>(this, m_uPos - a_Count, a_Count);
		}
		else
		{
			//BAD_EXCEPTION("Cuda data stream malloc failure, %d elements requested, %d available.", a_Count, m_uLength - m_uPos)
			//return e_BufferReference<T, T>();
			unsigned int newLength = m_uPos + a_Count;
			cudaFree(device);
			T* newHost = (T*)::malloc(m_uHostBlockSize * newLength);
			::memcpy(newHost, host, m_uPos * m_uHostBlockSize);
			free(host);
			host = newHost;
			m_uLength = newLength;
			CUDA_MALLOC(&device, sizeof(T) * newLength);
			cudaMemset(device, 0, sizeof(T) * newLength);
			Invalidate(0, m_uPos);
			return malloc(a_Count);
		}
		Invalidate(r);
		return r;
	}
	e_BufferReference<T, T> malloc(e_BufferReference<T, T> r, bool copyToNew = true)
	{
		e_BufferReference<T, T> r2 = malloc(r.getLength());
		if(copyToNew)
			memcpy(r, r2);
		return r2;
	}
	///Copies dest->length bytes from source to dest, on the host aswell as the device
	void memcpy(e_BufferReference<T, T> source, e_BufferReference<T, T> dest)
	{
		if(source.getLength() < dest.getLength() || source.m_pStream != this || dest.m_pStream != this)
			throw 1;
		::memcpy(dest.atH(dest.getIndex()), source.atH(source.getIndex()), m_uHostBlockSize * dest.getLength());
		Invalidate(dest);
	}
	void dealloc(e_BufferReference<T, T> a_Ref)
	{
		if(a_Ref.getIndex() + a_Ref.getLength() == m_uPos)
			m_uPos -= a_Ref.getLength();
		else
		{
			for(unsigned int i = 0; i < m_sDeallocated.size(); i++)
				if(m_sDeallocated[i].first == a_Ref.getIndex() + a_Ref.getLength())
				{
					m_sDeallocated[i].first = a_Ref.getIndex();
					m_sDeallocated[i].second += a_Ref.getLength();
					return;
				}
			m_sDeallocated.push_back(std::make_pair(a_Ref.getIndex(), a_Ref.getLength()));
		}
	}
	void dealloc(unsigned int i, unsigned int j = 1)
	{
		dealloc(operator()(i, j));
	}
	void Invalidate()
	{
		m_sInvalidated.clear();
		m_sInvalidated.push_back(std::make_pair(0U, m_uPos));
	}
	void Invalidate(e_BufferReference<T, T> a_Ref)
	{	
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			if(m_sInvalidated[i].first <= a_Ref.getIndex() && m_sInvalidated[i].second + m_sInvalidated[i].first >= a_Ref.getIndex() + a_Ref.getLength())
				return;
			else if(m_sInvalidated[i].first <= a_Ref.getIndex() && m_sInvalidated[i].first + m_sInvalidated[i].second >= a_Ref.getIndex())
			{
				m_sInvalidated[i].second = a_Ref.getIndex() + a_Ref.getLength() - m_sInvalidated[i].first;
				return;
			}
		}
		m_sInvalidated.push_back(std::make_pair(a_Ref.getIndex(), a_Ref.getLength()));
	}
	void Invalidate(unsigned int i, unsigned int j = 1)
	{
		Invalidate(operator()(i, j));
	}
	void UpdateInvalidated()
	{
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			int q = sizeof(T), f = m_sInvalidated[i].first, n = q * m_sInvalidated[i].second;
			cudaMemcpy(device + f, host + f, n, cudaMemcpyHostToDevice);
		}
		m_sInvalidated.clear();
	}
	template<typename CBT> void UpdateInvalidatedCB(CBT& cbf)
	{
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			int q = sizeof(T), f = m_sInvalidated[i].first, n = q * m_sInvalidated[i].second;
			for(unsigned int j = 0; j < m_sInvalidated[i].second; j++)
				cbf(operator()(f + j));
			cudaMemcpy(device + f, host + f, n, cudaMemcpyHostToDevice);
		}
		m_sInvalidated.clear();
	}
	unsigned int getLength()
	{
		return m_uLength;
	}
	e_BufferReference<T, T> operator()(unsigned int i = 0, unsigned int l = 1)
	{
		if (i >= m_uLength)
			BAD_EXCEPTION("Invalid index of %d, length is %d", i, m_uLength)
		return e_BufferReference<T, T>(this, i, l);
	}
	unsigned int NumUsedElements()
	{
		return m_uPos;
	}
	e_BufferReference<T, T> UsedElements()
	{
		return e_BufferReference<T, T>(this, 0, m_uPos);
	}
	unsigned int getSizeInBytes()
	{
		return m_uLength * sizeof(T);
	}
	e_KernelBuffer<T> getKernelData(bool devicePointer = true)
	{
		e_KernelBuffer<T> r;
		r.Data = devicePointer ? device : host;
		r.Length = m_uLength;
		r.UsedCount = m_uPos;
		return r;
	}
	e_BufferReference<T, T> translate(const T* val)
	{
		unsigned long long t0 = ((unsigned long long)val - (unsigned long long)host) / sizeof(T), t1 = ((unsigned long long)val - (unsigned long long)device) / sizeof(T);
		unsigned int i = t0 < m_uLength ? t0 : t1;
		return e_BufferReference<T, T>(this, i, 1);
	}
	T* getDeviceMapped(int i) const
	{
		return host + i;
	}
};

template<typename T> class e_Stream : public e_Buffer<T, T>
{
public:
	e_Stream(unsigned int N)
		: e_Buffer<T, T>(N)
	{
	}
	e_Stream(InputStream& a_In)
		: e_Buffer<T, T>(a_In)
	{

	}
};
/*
template<typename T> class e_CachedStream : public e_CachedBuffer<T, T>
{
public:
	e_CachedStream(unsigned int N)
		: e_CachedBuffer<T, T>(N)
	{

	}
	virtual void UpdateInvalidated()
	{
		for(unsigned int i = 0; i < m_sInvalidated.size(); i++)
		{
			int q = sizeof(T), f = m_sInvalidated[i].first, n = q * m_sInvalidated[i].second;
			cudaMemcpy(device + f, host + f, n, cudaMemcpyHostToDevice);
		}
		m_sInvalidated.clear();
	} 
};*/

