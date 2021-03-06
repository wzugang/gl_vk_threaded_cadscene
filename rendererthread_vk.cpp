/* Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Contact ckubisch@nvidia.com (Christoph Kubisch) for feedback */


#include <assert.h>
#include <algorithm>
#include <queue>
#include "renderer.hpp"
#include <main.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <nv_helpers/spin_mutex.hpp>
#include "resources_vk.hpp"

#include <nv_math/nv_math_glsltypes.h>

using namespace nv_math;
#include "common.h"


namespace csfthreaded
{

  //////////////////////////////////////////////////////////////////////////

  
  class RendererThreadedVK: public Renderer {
  public:
    enum Mode {
      MODE_CMD_MAINSUBMIT,
      MODE_CMD_WORKERSUBMIT,
    };


    class TypeCmd : public Renderer::Type 
    {
      bool isAvailable() const
      {
        return ResourcesVK::isAvailable();
      }
      const char* name() const
      {
        return "Vulkan MT cmd main process";
      }
      Renderer* create() const
      {
        RendererThreadedVK* renderer = new RendererThreadedVK();
        renderer->m_mode = MODE_CMD_MAINSUBMIT;
        return renderer;
      }
      unsigned int priority() const 
      {
        return 10;
      }

      Resources* resources()
      {
        return ResourcesVK::get();
      }
    };

    class TypeCmdSlave : public Renderer::Type 
    {
      bool isAvailable() const
      {
        return ResourcesVK::isAvailable();
      }
      const char* name() const
      {
        return "Vulkan MT cmd worker process";
      }
      Renderer* create() const
      {
        RendererThreadedVK* renderer = new RendererThreadedVK();
        renderer->m_mode = MODE_CMD_WORKERSUBMIT;
        return renderer;
      }
      unsigned int priority() const 
      {
        return 10;
      }

      Resources* resources()
      {
        return ResourcesVK::get();
      }
    };

  public:

    void init(const CadScene* NV_RESTRICT scene, Resources* resources, const Renderer::Config& config);
    void deinit();
    void draw(ShadeType shadetype, Resources* NV_RESTRICT resources, const Resources::Global& global, nv_helpers::Profiler& profiler);
    

    Mode            m_mode;

    RendererThreadedVK()
      : m_mode(MODE_CMD_MAINSUBMIT) 
    {

    }

  private:

    struct ShadeCommand {
      std::vector<VkCommandBuffer>     cmdbuffers;
    };


    struct ThreadJob {
      RendererThreadedVK*           renderer;
      int                           index;

      nv_helpers_vk::RingCmdPool    m_pool;

      int                           m_frame;
      std::condition_variable       m_hasWorkCond;
      std::mutex                    m_hasWorkMutex;
      volatile int                  m_hasWork;

      size_t                        m_scIdx;
      std::vector<ShadeCommand*>    m_scs;


      void resetFrame(){
        m_scIdx = 0;
      }

      ShadeCommand* getFrameCommand() {
        if (m_scIdx+1 > m_scs.size()){
          ShadeCommand* sc = new ShadeCommand;
          m_scIdx++;
          m_scs.push_back(sc);
          return sc;
        }
        else{
          return m_scs[m_scIdx++];
        }
      }
    };


    std::vector<DrawItem>           m_drawItems;
    ResourcesVK* NV_RESTRICT       m_resources;
    int                             m_numThreads;

    int                             m_workingSet;
    ShadeType                       m_shade;
    int                             m_frame;

    ThreadJob*                      m_jobs;

    volatile int                    m_ready;
    volatile int                    m_stopThreads;
    volatile size_t                 m_numCurItems;

    std::condition_variable         m_readyCond;
    std::mutex                      m_readyMutex;

    size_t                          m_numEnqueues;
    std::queue<ShadeCommand*>       m_drawQueue;

    nv_helpers::spin_mutex          m_workMutex;
    std::mutex                      m_drawMutex;
    std::condition_variable         m_drawMutexCondition;

#if USE_THREADED_SECONDARIES
    VkCommandBuffer                 m_primary;
#endif

    static void threadMaster( void* arg  )
    {
      ThreadJob* job = (ThreadJob*) arg;
      job->renderer->RunThread( job->index );
    }

    bool getWork_ts( size_t &start, size_t &num )
    {
      std::lock_guard<nv_helpers::spin_mutex> lock(m_workMutex);
      bool hasWork = false;

      const size_t chunkSize = m_workingSet;
      size_t total = m_drawItems.size();

      if (m_numCurItems < total){
        size_t batch = std::min(total-m_numCurItems,chunkSize);
        start = m_numCurItems;
        num   = batch;
        m_numCurItems += batch;
        hasWork = true;
      }
      else{
        hasWork = false;
        start = 0;
        num = 0;
      }

      return hasWork;
    }

    void          RunThread( int index );
    unsigned int  RunThreadFrame(ShadeType shadetype, ThreadJob& job);

    void enqueueShadeCommand_ts( ShadeCommand *sc );
    void submitShadeCommand_ts( ShadeCommand *sc );

    template <ShadeType shadetype, bool sorted>
    void GenerateCmdBuffers(ShadeCommand& sc, nv_helpers_vk::RingCmdPool& pool, const DrawItem* NV_RESTRICT drawItems, size_t num, const ResourcesVK* NV_RESTRICT res )
    {
      const CadScene* NV_RESTRICT scene = m_scene;
      bool solidwire = (shadetype == SHADE_SOLIDWIRE);

      int lastMaterial = -1;
      int lastGeometry = -1;
      int lastMatrix   = -1;
      bool lastSolid   = true;

      sc.cmdbuffers.clear();

      // TODO could recycle pool's allocated commandbuffers and not free them
#if USE_THREADED_SECONDARIES
      VkCommandBuffer  cmd = pool.createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
      res->cmdBegin(cmd, true, false, true);
#else
      VkCommandBuffer  cmd = pool.createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
      res->cmdBegin(cmd, true, true, false);
      res->cmdPipelineBarrier(cmd);
      res->cmdBeginRenderPass(cmd, false);
#endif
      res->cmdDynamicState(cmd);

      VkPipeline solidPipeline = solidwire ? res->pipes.line_tris : res->pipes.tris;
      VkPipeline nonSolidPipeline = res->pipes.line;

      if (num) {
        bool solid = shadetype == SHADE_SOLID ? true : drawItems[0].solid;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
          solid ? solidPipeline : nonSolidPipeline);
#if UNIFORMS_TECHNIQUE == UNIFORMS_MULTISETSDYNAMIC || UNIFORMS_TECHNIQUE == UNIFORMS_MULTISETSSTATIC
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.getPipeLayout(),
          UBO_SCENE, 1, res->m_drawing.getSets(UBO_SCENE), 0, NULL);
#endif
#if UNIFORMS_TECHNIQUE == UNIFORMS_PUSHCONSTANTS_RAW || UNIFORMS_TECHNIQUE == UNIFORMS_PUSHCONSTANTS_INDEX
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.getPipeLayout(),
          UBO_SCENE, 1, res->m_drawing.getSets(0), 0, NULL);
#endif
        lastSolid = solid;
      }

      bool first = true;
      const DrawItem* drawItemsEnd = drawItems + num;
      while (drawItems != drawItemsEnd) {
        const DrawItem& di = *drawItems++;

        if (shadetype == SHADE_SOLID && !di.solid){
          if (sorted) break;
          continue;
        }

        if (shadetype == SHADE_SOLIDWIRE && di.solid != lastSolid) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            di.solid ? solidPipeline : nonSolidPipeline);

          lastSolid = di.solid;
        }

        if (lastGeometry != di.geometryIndex){
          const ResourcesVK::Geometry &glgeo = res->m_geometry[di.geometryIndex];

          vkCmdBindVertexBuffers(cmd, 0, 1, &glgeo.vbo.buffer, &glgeo.vbo.offset);
          vkCmdBindIndexBuffer  (cmd, glgeo.ibo.buffer, glgeo.ibo.offset, VK_INDEX_TYPE_UINT32);

          lastGeometry = di.geometryIndex;
        }

      ///////////////////////////////////////////////////////////////////////////////////////////
      #if UNIFORMS_TECHNIQUE == UNIFORMS_MULTISETSDYNAMIC || UNIFORMS_TECHNIQUE == UNIFORMS_MULTISETSSTATIC
        if (lastMatrix != di.matrixIndex)
        {
        #if UNIFORMS_TECHNIQUE == UNIFORMS_MULTISETSDYNAMIC
          uint32_t offset = di.matrixIndex    * res->m_alignedMatrixSize;
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.getPipeLayout(),
            UBO_MATRIX, 1, res->m_drawing.getSets(UBO_MATRIX), 1, &offset);
        #else
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.getPipeLayout(),
            UBO_MATRIX, 1, res->m_drawing.getSets(UBO_MATRIX) + di.matrixIndex, 0, NULL);
        #endif
          lastMatrix = di.matrixIndex;
        }

        if (lastMaterial != di.materialIndex)
        {
        #if UNIFORMS_TECHNIQUE == UNIFORMS_MULTISETSDYNAMIC
          uint32_t offset = di.materialIndex    * res->m_alignedMaterialSize;
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.getPipeLayout(),
            UBO_MATERIAL, 1, res->m_drawing.getSets(UBO_MATERIAL), 1, &offset);
        #else
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.getPipeLayout(),
            UBO_MATERIAL, 1, res->m_drawing.getSets(UBO_MATERIAL) + di.materialIndex, 0, NULL);
        #endif
          lastMaterial = di.materialIndex;
        }
      ///////////////////////////////////////////////////////////////////////////////////////////
      #elif UNIFORMS_TECHNIQUE == UNIFORMS_ALLDYNAMIC || UNIFORMS_TECHNIQUE == UNIFORMS_SPLITDYNAMIC

        if (lastMaterial != di.materialIndex ||
            lastMatrix   != di.matrixIndex)
        {
        #if UNIFORMS_TECHNIQUE == UNIFORMS_ALLDYNAMIC
          uint32_t offsets[UBOS_NUM];
          offsets[UBO_SCENE]    = 0;
          offsets[UBO_MATRIX]   = di.matrixIndex    * res->m_alignedMatrixSize;
          offsets[UBO_MATERIAL] = di.materialIndex  * res->m_alignedMaterialSize;

        #elif UNIFORMS_TECHNIQUE == UNIFORMS_SPLITDYNAMIC
          uint32_t offsets[UBOS_NUM-1];
          offsets[UBO_MATRIX-1]   = di.matrixIndex    * res->m_alignedMatrixSize;
          offsets[UBO_MATERIAL-1] = di.materialIndex  * res->m_alignedMaterialSize;
        #endif
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res->m_drawing.pipelineLayout,
            0, 1, res->m_drawing.getSets(0), sizeof(offsets)/sizeof(offsets[0]),offsets);

          lastMaterial = di.materialIndex;
          lastMatrix   = di.matrixIndex;
        }
      ///////////////////////////////////////////////////////////////////////////////////////////
      #elif UNIFORMS_TECHNIQUE == UNIFORMS_PUSHCONSTANTS_RAW
        if (lastMatrix != di.matrixIndex)
        {
          vkCmdPushConstants(cmd, res->m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(ObjectData), &scene->m_matrices[di.matrixIndex]);

          lastMatrix = di.matrixIndex;
        }

        if (lastMaterial != di.materialIndex)
        {
          vkCmdPushConstants(cmd, res->m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,sizeof(ObjectData),sizeof(MaterialData), &scene->m_materials[di.materialIndex]);

          lastMaterial = di.materialIndex;
        }
      ///////////////////////////////////////////////////////////////////////////////////////////
      #elif UNIFORMS_TECHNIQUE == UNIFORMS_PUSHCONSTANTS_INDEX
        if (lastMatrix != di.matrixIndex)
        {
          vkCmdPushConstants(cmd, res->m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(int), &di.matrixIndex);

          lastMatrix = di.matrixIndex;
        }

        if (lastMaterial != di.materialIndex)
        {
          vkCmdPushConstants(cmd, res->m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,sizeof(int),sizeof(int), &di.materialIndex);

          lastMaterial = di.materialIndex;
        }
      ///////////////////////////////////////////////////////////////////////////////////////////
      #endif
      
        // drawcall
        vkCmdDrawIndexed(cmd, di.range.count, 1, (uint32_t)(di.range.offset / sizeof(uint32_t)), 0, 0);
      }

      if (cmd){
#if !USE_THREADED_SECONDARIES
        vkCmdEndRenderPass(cmd);
#endif
        vkEndCommandBuffer(cmd);
        sc.cmdbuffers.push_back(cmd);
      }
    }

    void GenerateCmdBuffers(ShadeCommand& sc, ShadeType shadeType, nv_helpers_vk::RingCmdPool& pool, const DrawItem* NV_RESTRICT drawItems, size_t num, const ResourcesVK* NV_RESTRICT res)
    {
      if (m_config.sorted)
      {
        switch (shadeType) {
        case SHADE_SOLID:
          GenerateCmdBuffers<SHADE_SOLID, true>(sc, pool, drawItems, num, res);
          break;
        case SHADE_SOLIDWIRE:
          GenerateCmdBuffers<SHADE_SOLIDWIRE, true>(sc, pool, drawItems, num, res);
          break;
        }
      }
      else {
        switch (shadeType) {
        case SHADE_SOLID:
          GenerateCmdBuffers<SHADE_SOLID, false>(sc, pool, drawItems, num, res);
          break;
        case SHADE_SOLIDWIRE:
          GenerateCmdBuffers<SHADE_SOLIDWIRE, false>(sc, pool, drawItems, num, res);
          break;
        }
      }
    }

   };


  static RendererThreadedVK::TypeCmd s_type_cmdmain_xgl;
  static RendererThreadedVK::TypeCmdSlave s_type_cmdslave_xgl;

  void RendererThreadedVK::init(const CadScene* NV_RESTRICT scene, Resources* resources, const Renderer::Config& config)
  {
    const ResourcesVK* res = (const ResourcesVK*) resources;
    m_scene = scene;

    fillDrawItems(m_drawItems, config, true, true);

    LOGI("drawitems: %d\n", uint32_t(m_drawItems.size()));

    if (config.sorted){
      std::sort(m_drawItems.begin(),m_drawItems.end(),DrawItem_compare_groups);
    }

    m_resources  = (ResourcesVK*) resources;
    m_numThreads = config.threads;

    // make jobs
    m_ready = 0;
    m_jobs = new ThreadJob[m_numThreads];
    m_stopThreads = 0;

    for (int i = 0; i < m_numThreads; i++)
    {
      ThreadJob& job = m_jobs[i];
      job.index = i;
      job.renderer = this;
      job.m_hasWork = -1;
      job.m_frame = 0;

      job.m_pool.init(res->m_device, res->m_queueFamily);

      s_threadpool.activateJob( i, threadMaster, &m_jobs[i]);
    }

    m_frame = 0;
  }

  void RendererThreadedVK::deinit()
  {
    m_stopThreads = 1;
    m_ready = 0;

    NV_BARRIER();
    for (int i = 0; i < m_numThreads; i++){
      std::unique_lock<std::mutex> lock(m_jobs[i].m_hasWorkMutex);
      m_jobs[i].m_hasWork = m_frame;
      m_jobs[i].m_hasWorkCond.notify_one();
    }
    m_drawMutexCondition.notify_all();

    std::this_thread::yield();

    {
      std::unique_lock<std::mutex> lock(m_readyMutex);
      while (m_ready < m_numThreads){
        m_readyCond.wait(lock);
      }
    }

    NV_BARRIER();

    for (int i = 0; i < m_numThreads; i++)
    {
      for (size_t s = 0; s < m_jobs[i].m_scs.size(); s++ ){
        delete m_jobs[i].m_scs[s];
      }
      m_jobs[i].m_pool.deinit();
    }

    delete [] m_jobs;

    m_drawItems.clear();
  }

  void RendererThreadedVK::enqueueShadeCommand_ts( ShadeCommand *sc )
  {
    std::unique_lock<std::mutex> lock(m_drawMutex);

    m_drawQueue.push(sc);
    m_drawMutexCondition.notify_one();
  }


  void RendererThreadedVK::submitShadeCommand_ts( ShadeCommand *sc )
  {
    {
      std::unique_lock<std::mutex> lock(m_drawMutex);
#if USE_THREADED_SECONDARIES
      vkCmdExecuteCommands(m_primary, (uint32_t)sc->cmdbuffers.size(), sc->cmdbuffers.data());
#else
      m_resources->submissionEnqueue(sc->cmdbuffers.size(), sc->cmdbuffers.data());
#endif
      NV_BARRIER();
    }

    sc->cmdbuffers.clear();
  }


  unsigned int RendererThreadedVK::RunThreadFrame(ShadeType shadetype, ThreadJob& job) 
  {
    unsigned int dispatches = 0;

    bool   first = true;
    size_t tnum  = 0;
    size_t begin = 0;
    size_t num   = 0;

    size_t offset  = 0;

    int subframe = m_frame % nv_helpers_vk::MAX_RING_FRAMES;

    job.resetFrame();
    job.m_pool.setCycle(subframe);

    while (getWork_ts(begin,num))
    {
      ShadeCommand* sc = job.getFrameCommand();
      GenerateCmdBuffers(*sc, shadetype, job.m_pool, &m_drawItems[begin], num, m_resources);
      if (!sc->cmdbuffers.empty()){
        if (m_mode == MODE_CMD_MAINSUBMIT){
          enqueueShadeCommand_ts(sc);
        }
        else if (m_mode == MODE_CMD_WORKERSUBMIT){
          submitShadeCommand_ts(sc);
        }
        dispatches += 1;
      }
      tnum += num;
    }

    // NULL signals we are done
    enqueueShadeCommand_ts(NULL);

    return dispatches;
  }

  void RendererThreadedVK::RunThread( int tid )
  {
    ThreadJob & job  = m_jobs[tid];
    ShadeType shadetype;

    double timeWork = 0;
    double timeFrame = 0;
    int timerFrames = 0;
    size_t dispatches = 0;

    double timePrint = NVPWindow::sysGetTime();

    while(!m_stopThreads)
    {
      //NV_BARRIER();

      double beginFrame = NVPWindow::sysGetTime();
      timeFrame -= NVPWindow::sysGetTime();
      {
        std::unique_lock<std::mutex> lock(job.m_hasWorkMutex);
        while(job.m_hasWork != job.m_frame){
          job.m_hasWorkCond.wait(lock);
        }

        shadetype = m_shade;
      }

      if (m_stopThreads){
        break;
      }

      double beginWork = NVPWindow::sysGetTime();
      timeWork -= NVPWindow::sysGetTime();

      dispatches += RunThreadFrame(shadetype,job);

      job.m_frame++;

      timeWork += NVPWindow::sysGetTime();

      double currentTime = NVPWindow::sysGetTime();
      timeFrame += currentTime;

      timerFrames++;

      if (timerFrames && (currentTime - timePrint) > 2.0){
        timeFrame /= double(timerFrames);
        timeWork  /= double(timerFrames);

        timeFrame *= 1000000.0;
        timeWork  *= 1000000.0;

        timePrint = currentTime;

        uint32_t avgdispatch = uint32_t(double(dispatches)/double(timerFrames));

#if PRINT_TIMER_STATS
        LOGI("thread %d: work %6d [us] dispatches %5d\n", tid, uint32_t(timeWork), uint32_t(avgdispatch));
#endif
        timeFrame = 0;
        timeWork = 0;

        timerFrames = 0;
        dispatches = 0;
      }
    }

    {
      std::unique_lock<std::mutex> lock(m_readyMutex);
      m_ready++;
      m_readyCond.notify_all();
    }
  }

  void RendererThreadedVK::draw(ShadeType shadetype, Resources* NV_RESTRICT resources, const Resources::Global& global, nv_helpers::Profiler& profiler)
  {
    const CadScene* NV_RESTRICT scene = m_scene;
    ResourcesVK* res = (ResourcesVK*)resources;

    // generic state setup
    {
      VkCommandBuffer cmd = res->createTempCmdBuffer();
      vkCmdUpdateBuffer(cmd, res->buffers.scene, 0, sizeof(SceneData), (const uint32_t*)&global.sceneUbo);

      res->cmdPipelineBarrier(cmd);
      res->cmdBeginRenderPass(cmd, true, USE_THREADED_SECONDARIES ? true : false);
      
#if USE_THREADED_SECONDARIES
      m_primary = cmd;
#else
      vkCmdEndRenderPass(cmd);
      vkEndCommandBuffer(cmd);

      res->submissionEnqueue(cmd);
#endif
    }

    m_workingSet   = global.workingSet;
    m_shade        = shadetype;
    m_numCurItems  = 0;
    m_numEnqueues  = 0;

    // generate & cmdbuffers in parallel

    NV_BARRIER();

    // start to dispatch threads
    for (int i = 0; i < m_numThreads; i++){
      {
        std::unique_lock<std::mutex> lock(m_jobs[i].m_hasWorkMutex);
        m_jobs[i].m_hasWork = m_frame;
      }
      m_jobs[i].m_hasWorkCond.notify_one();
    }

    // dispatch drawing here
    {
      int numTerminated = 0;
      while (true) {
        bool hadEntry = false;
        ShadeCommand* sc = NULL;
        {
          std::unique_lock<std::mutex> lock(m_drawMutex);
          if (m_drawQueue.empty()) {
            m_drawMutexCondition.wait(lock);
          }
          if (!m_drawQueue.empty()) {

            sc = m_drawQueue.front();
            m_drawQueue.pop();

            hadEntry = true;
          }
        }

        if (hadEntry){
          if (sc){
            m_numEnqueues++;
#if USE_THREADED_SECONDARIES
            vkCmdExecuteCommands(m_primary, (uint32_t)sc->cmdbuffers.size(), &sc->cmdbuffers[0]);
#else
            res->submissionEnqueue(sc->cmdbuffers.size(), sc->cmdbuffers.data());
#endif
            sc->cmdbuffers.clear();
          }
          else{
            numTerminated++;
          }
        }

        if (numTerminated == m_numThreads){
          break;
        }
        std::this_thread::yield();
      }
    }

    NV_BARRIER();

#if USE_THREADED_SECONDARIES
    {
      vkCmdEndRenderPass(m_primary);
      vkEndCommandBuffer(m_primary);

      res->submissionEnqueue(m_primary);
    }
#endif

    m_frame++;
  }

}


