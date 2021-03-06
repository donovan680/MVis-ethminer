/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file EthashGPUMiner.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 *
 * Determines the PoW algorithm.
 */

#if ETH_ETHASHCL || !ETH_TRUE

#include "EthashGPUMiner.h"
#include <thread>
#include <chrono>
#include <libethash-cl/ethash_cl_miner.h>
#include "ethminer/MultiLog.h"

using namespace std;
using namespace dev;
using namespace eth;

namespace dev
{
namespace eth
{

/*-----------------------------------------------------------------------------------
* class EthashCLHook
*----------------------------------------------------------------------------------*/

class EthashCLHook: public ethash_cl_miner::search_hook
{
public:

	EthashCLHook(EthashGPUMiner* _owner): m_owner(_owner) {}
	EthashCLHook(EthashCLHook const&) = delete;

	void abort()
	{
		{
			UniqueGuard l(x_all);
			if (m_aborted)
				return;
//		cdebug << "Attempting to abort";

			
			m_abort = true;
		}
		// m_abort is true so now searched()/found() will return true to abort the search.
		// we hang around on this thread waiting for them to point out that they have aborted since
		// otherwise we may end up deleting this object prior to searched()/found() being called.
		m_aborted.wait(true);
	}

	void reset()
	{
		UniqueGuard l(x_all);
		m_aborted = m_abort = false;
	}

protected:
	virtual bool found(uint64_t const* _nonces, uint32_t _count) override
	{
		LogF << "Trace: EthashCLHook::found, miner[" << m_owner->m_index << "], count=" << _count;
		for (uint32_t i = 0; i < _count; ++i)
			if (m_owner->report(_nonces[i]))
				return (m_aborted = true);
		return m_owner->shouldStop();
	}

	virtual bool searched(uint32_t _count, uint64_t _hashSample, uint64_t _bestHash) override
	{
		UniqueGuard l(x_all);
		bool shouldStop = m_abort || m_owner->shouldStop();
		m_owner->setCurrentHash(_hashSample);
		m_owner->setBestHash(_bestHash);	// this best hash might be the same as the last one, but we're not time critical here.
		return (m_aborted = shouldStop);
	}

	virtual bool shouldStop() override
	{
		UniqueGuard l(x_all);
		if (m_abort || m_owner->shouldStop())
			return (m_aborted = true);
		return false;
	}

private:
	Mutex x_all;
	bool m_abort = false;
	Notified<bool> m_aborted = {true};
	EthashGPUMiner* m_owner = nullptr;
};

}
}


/*-----------------------------------------------------------------------------------
* class EthashGPUMiner
*----------------------------------------------------------------------------------*/

unsigned EthashGPUMiner::s_platformId = 0;
unsigned EthashGPUMiner::s_deviceId = 0;
unsigned EthashGPUMiner::s_numInstances = 0;
int EthashGPUMiner::s_devices[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

EthashGPUMiner::EthashGPUMiner(Farm* _farm, unsigned _index):
	GenericMiner<EthashProofOfWork>(_farm, _index),
	Worker("openclminer" + toString(index())),
	m_hook(new EthashCLHook(this))
{
}

EthashGPUMiner::~EthashGPUMiner()
{
	pause();
	delete m_miner;
	delete m_hook;
}

bool EthashGPUMiner::report(uint64_t _nonce)
{
	Nonce n = (Nonce)(u64)_nonce;
	WorkPackage w = work();
	EthashProofOfWork::Result r = EthashAux::eval(w.seedHash, w.headerHash, n);
	uint64_t hash = upper64OfHash(r.value);
	if (r.value < w.boundary)
		return submitProof(Solution{n, r.mixHash});
	else
	{
		if (m_closeHit > 0 && hash < m_closeHit)
		{
			unsigned work = std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - m_lastCloseHit).count();
			m_farm->reportCloseHit(hash, work, m_index);
			m_lastCloseHit = SteadyClock::now();
			return false;
		}
		LogB << "Hash fault : nonce = 0x" << std::hex << _nonce
			<< ", headerHash = " << w.headerHash.hex().substr(0, 8) << ", [device:" << m_index << "]";
		m_farm->reportHashFault(m_index);
	}
	return false;
}

void EthashGPUMiner::resetBestHash() 
{ 
	GenericMiner::resetBestHash();
	Guard l(m_miner->x_bestHash);
	m_miner->m_bestHash = ~uint64_t(0);
}

void EthashGPUMiner::kickOff()
{
	LogF << "Trace: EthashGPUMiner::kickOff, miner[" << m_index << "]";
	m_hook->reset();
	startWorking();
}

void EthashGPUMiner::workLoop()
{
	LogF << "Trace: EthashGPUMiner::workLoop-1, miner[" << m_index << "]";
	try {
		// take local copy of work since it may end up being overwritten by kickOff/pause.
		WorkPackage w = work();
		if (!m_miner || m_minerSeed != w.seedHash)
		{
			LogF << "Trace: EthashGPUMiner::workLoop-2, miner[" << m_index << "]";
			if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
			{
				while (s_dagLoadIndex < index()) {
					this_thread::sleep_for(chrono::seconds(1));
				}
			}

			LogS << "Initialising miner for " << w.seedHash.hex().substr(0, 8);
			m_minerSeed = w.seedHash;

			delete m_miner;
			m_miner = new ethash_cl_miner(this);

			m_device = s_devices[index()] > -1 ? s_devices[index()] : index();

			EthashAux::LightType light;
			light = EthashAux::light(w.seedHash);
			bytesConstRef lightData = light->data();

			if (!m_miner->init(light->light, lightData.data(), lightData.size(), s_platformId, m_device))
				throw cl::Error(-1, "cl_miner.init failed!");

			s_dagLoadIndex++;
		}

		m_farm->setIsMining(true);

		uint64_t startN = 0;
		if (w.exSizeBits >= 0)
			startN = w.startNonce | ((uint64_t)index() << (64 - 4 - w.exSizeBits)); // this can support up to 16 devices
		uint64_t threshold = max(m_closeHit, upper64OfHash(w.boundary));
		m_miner->search(w.headerHash, threshold, *m_hook, (w.exSizeBits >= 0), startN);
	}
	catch (cl::Error const& _e)
	{
		delete m_miner;
		m_miner = nullptr;
		LogB << "Error GPU mining: " << _e.what() << "(" << _e.err() << ")";
	}
	LogF << "Trace: EthashGPUMiner::workLoop-exit, miner[" << m_index << "]";
}

void EthashGPUMiner::pause()
{
	LogF << "Trace: EthashGPUMiner::pause, miner[" << m_index << "]";
	m_hook->abort();
	stopWorking();
}

void EthashGPUMiner::checkHash(uint64_t _hash, uint64_t _nonce, h256 _header)
{
	WorkPackage w = work();
	h256 header = w.headerHash;
	h256 seed = w.seedHash;

	if (_header != header)
		// a new work package just came in so we'll skip this check.
		return;
	Nonce n = (Nonce) (u64) _nonce;
	EthashProofOfWork::Result r = EthashAux::eval(seed, header, n);
	if (_hash != upper64OfHash(r.value))
	{
		LogB << "Hash fault : nonce = 0x" << std::hex << _nonce
			<< ", headerHash = " << header.hex().substr(0, 8) << ", [device:" << m_index << "]";
		m_farm->reportHashFault(m_index);
	}
}


std::string EthashGPUMiner::platformInfo()
{
	return ethash_cl_miner::platform_info(s_platformId, s_deviceId);
}

unsigned EthashGPUMiner::getNumDevices()
{
	return ethash_cl_miner::getNumDevices(s_platformId);
}

void EthashGPUMiner::listDevices()
{
	return ethash_cl_miner::listDevices();
}

bool EthashGPUMiner::configureGPU(
	unsigned _localWorkSize,
	unsigned _workSizeMultiplier,
	unsigned _platformId,
	unsigned _deviceId,
	bool _allowCPU,
	unsigned _extraGPUMemory,
	uint64_t _currentBlock,
	unsigned _dagLoadMode,
	unsigned _dagCreateDevice
)
{
	s_dagLoadMode = _dagLoadMode;
	s_dagCreateDevice = _dagCreateDevice;

	s_platformId = _platformId;
	s_deviceId = _deviceId;

	_localWorkSize = ((_localWorkSize + 7) / 8) * 8;

	if (!ethash_cl_miner::configureGPU(
			_platformId,
			_localWorkSize,
			_workSizeMultiplier * _localWorkSize,
			_allowCPU,
			_extraGPUMemory,
			_currentBlock
			)
	)
	{
		LogB << "No GPU device with sufficient memory was found. Can't GPU mine. Remove the -G argument";
		return false;
	}
	return true;
}

void EthashGPUMiner::setThrottle(int _percent)
{
	if (m_farm->isMining())
	{
		m_throttle = _percent;
		m_miner->setThrottle(_percent);
	}
}

void dev::eth::EthashGPUMiner::exportDAG(unsigned _block) 
{
	ethash_cl_miner* miner = new ethash_cl_miner(NULL);

	h256 seedHash = EthashAux::seedHash(_block);

	EthashAux::LightType light;
	light = EthashAux::light(seedHash);
	bytesConstRef lightData = light->data();

	if (!miner->init(light->light, lightData.data(), lightData.size(), s_platformId, 0))
		throw cl::Error(-1, "cl_miner.init failed!");

	miner->exportDAG(seedHash.hex().substr(0, 16));

}

#endif
