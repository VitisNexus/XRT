/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. - All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define XDP_PLUGIN_SOURCE

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <chrono>
#include <fstream>
#include <regex>

#include "shim/shim.h"

#include "core/common/device.h"
#include "core/common/message.h"

#include "core/include/xrt/xrt_bo.h"
#include "core/include/xrt/xrt_kernel.h"

#include "xdp/profile/plugin/ml_timeline/ve2/ml_timeline.h"
#include "xdp/profile/plugin/vp_base/utility.h"

namespace xdp {

  class VE2ResultBO
  {
    private:
      aiarm::shim* mDev;
      std::unique_ptr<xrt_core::buffer_handle> mBufHandle;
      uint32_t *mBOptr = nullptr;
      bool mNoUnmap = false;

      uint32_t* mapAndChk()
      {
        mBOptr = reinterpret_cast<uint32_t *>(mBufHandle->map(xrt_core::buffer_handle::map_type::write));
        if (!mBOptr)
          throw std::runtime_error("Failed mapping bo of " + std::to_string(size()) + "bytes.");
        return mBOptr;
      }

    public:
      VE2ResultBO(aiarm::shim* devHandle, size_t size)
        : mDev(devHandle)
      {
        xcl_bo_flags flags {0};
        flags.flags = XRT_BO_FLAGS_CACHEABLE;
        flags.access = XRT_BO_ACCESS_LOCAL;
        flags.dir = XRT_BO_ACCESS_READ_WRITE;
        flags.use = XRT_BO_USE_DEBUG;
        mBufHandle  = mDev->xclAllocBO(size, flags.all);
        mapAndChk();
      }

      ~VE2ResultBO()
      {
        if (!mNoUnmap)
          mBufHandle->unmap(mBOptr);
      }

      xrt_core::buffer_handle* get() { return mBufHandle.get(); }

      uint32_t *map() { return mBOptr; }

      void set_no_unmap() { mNoUnmap = true; }

      size_t size() { return mBufHandle->get_properties().size; }
  };

  MLTimelineVE2Impl::MLTimelineVE2Impl(VPDatabase*dB, uint32_t sz)
    : MLTimelineImpl(dB, sz)
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
              "Created ML Timeline Plugin for VE2 Device.");
  }

  MLTimelineVE2Impl::~MLTimelineVE2Impl()
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
              "In destructor for ML Timeline Plugin for VE2 Device.");
  }

  void MLTimelineVE2Impl::updateDevice(void* devH)
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
              "In MLTimelineVE2Impl::updateDevice");
    try {
      mResultBOHolder = std::make_unique<xdp::VE2ResultBO>(reinterpret_cast<aiarm::shim*>(devH), mBufSz);
      memset(mResultBOHolder->map(), 0, mBufSz);

    } catch (std::exception& e) {
      std::stringstream msg;
      msg << "Unable to create/initialize result buffer of size "
          << std::hex << mBufSz << std::dec
          << " Bytes for Record Timer Values. Cannot get ML Timeline info. " 
          << e.what() << std::endl;
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg.str());
      return;
    }
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
              "Allocated buffer In MLTimelineVE2Impl::updateDevice");
  }

  void MLTimelineVE2Impl::finishflushDevice(void* /*hwCtxImpl*/, uint64_t implId)
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
              "In MLTimelineVE2Impl::finishflushDevice");
    
    if (!mResultBOHolder)
      return;
  
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
              "Using Allocated buffer In MLTimelineVE2Impl::finishflushDevice");
              
//    mResultBOHolder->syncFromDevice();    
    uint32_t* ptr = mResultBOHolder->map();
      
    boost::property_tree::ptree ptTop;
    boost::property_tree::ptree ptHeader;
    boost::property_tree::ptree ptRecordTimerTS;

    // Header for JSON 
    ptHeader.put("date", xdp::getCurrentDateTime());
    ptHeader.put("time_created", xdp::getMsecSinceEpoch());

    boost::property_tree::ptree ptSchema;
    ptSchema.put("major", "1");
    ptSchema.put("minor", "1");
    ptSchema.put("patch", "0");
    ptHeader.add_child("schema_version", ptSchema);
    ptHeader.put("device", "Client");
    ptHeader.put("clock_freq_MHz", 1000);
    ptHeader.put("id_size", sizeof(uint32_t));
    ptHeader.put("cycle_size", 2*sizeof(uint32_t));
    ptHeader.put("buffer_size", mBufSz);
    ptTop.add_child("header", ptHeader);

    // Record Timer TS in JSON
    // Assuming correct Stub has been called and Write Buffer contains valid data
    
    uint32_t maxCount = mBufSz / RECORD_TIMER_ENTRY_SZ_IN_BYTES;
    // Each record timer entry has 32bit ID and 32bit AIE High Timer + 32bit AIE Low Timer value.

    uint32_t numEntries = maxCount;
    std::stringstream msg;
    msg << "A maximum of " << numEntries << " record can be accommodated in given buffer of bytes size 0x"
        << std::hex << mBufSz << std::dec << std::endl;
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", msg.str());

    if (numEntries <= maxCount) {
      for (uint32_t i = 0 ; i < numEntries; i++) {
        boost::property_tree::ptree ptIdTS;
        uint32_t id = *ptr;
        ptIdTS.put("id", *ptr);
        ptr++;

        uint64_t ts64 = *ptr;
        ts64 = ts64 << 32;
        ptr++;
        ts64 |= (*ptr);
        if (0 == ts64 && 0 == id) {
          // Zero value for Timestamp in cycles (and id too) indicates end of recorded data
          std::string msgEntries = "Got " + std::to_string(i) + " records in buffer.";
          xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", msgEntries);
          break;
        }
        ptIdTS.put("cycle", ts64);
        ptr++;

        ptRecordTimerTS.push_back(std::make_pair("", ptIdTS));
      }
    }    

    if (ptRecordTimerTS.empty()) {
      boost::property_tree::ptree ptEmpty;
      ptRecordTimerTS.push_back(std::make_pair("", ptEmpty));
    }
    ptTop.add_child("record_timer_ts", ptRecordTimerTS);

    // Write output file
    std::ostringstream oss;
    boost::property_tree::write_json(oss, ptTop);

    // Remove quotes from value strings
    std::regex reg("\\\"((-?[0-9]+\\.{0,1}[0-9]*)|(null)|())\\\"(?!\\:)");
    std::string result = std::regex_replace(oss.str(), reg, "$1");

    std::string outFName;
    if (0 == implId) {
      outFName = "record_timer_ts.json";
    } else {
      outFName = "record_timer_ts_" + std::to_string(implId) + ".json";
    }
    std::ofstream fOut;
    fOut.open(outFName);
    fOut << result;
    fOut.close();

    std::stringstream msg1;
    msg1 << "Finished writing " << outFName << " in MLTimelineVE2Impl::finishflushDevice." << std::endl;
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", msg1.str());
  
    /* Delete the result BO so that AIE Profile/Debug Plugins, if enabled,
     * can use their own Debug BO to capture their data.
     */
    mResultBOHolder.reset(nullptr);
  }
}
