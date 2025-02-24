/*
 * Copyright (C) 2018 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <ignition/common/Console.hh>

#include "ignition/rendering/RenderTypes.hh"
#include "ignition/rendering/ogre2/Ogre2ArrowVisual.hh"
#include "ignition/rendering/ogre2/Ogre2AxisVisual.hh"
#include "ignition/rendering/ogre2/Ogre2Camera.hh"
#include "ignition/rendering/ogre2/Ogre2Capsule.hh"
#include "ignition/rendering/ogre2/Ogre2Conversions.hh"
#include "ignition/rendering/ogre2/Ogre2DepthCamera.hh"
#include "ignition/rendering/ogre2/Ogre2GizmoVisual.hh"
#include "ignition/rendering/ogre2/Ogre2GpuRays.hh"
#include "ignition/rendering/ogre2/Ogre2Grid.hh"
#include "ignition/rendering/ogre2/Ogre2Light.hh"
#include "ignition/rendering/ogre2/Ogre2LightVisual.hh"
#include "ignition/rendering/ogre2/Ogre2LidarVisual.hh"
#include "ignition/rendering/ogre2/Ogre2Marker.hh"
#include "ignition/rendering/ogre2/Ogre2Material.hh"
#include "ignition/rendering/ogre2/Ogre2MeshFactory.hh"
#include "ignition/rendering/ogre2/Ogre2Node.hh"
#include "ignition/rendering/ogre2/Ogre2ParticleEmitter.hh"
#include "ignition/rendering/ogre2/Ogre2RayQuery.hh"
#include "ignition/rendering/ogre2/Ogre2RenderEngine.hh"
#include "ignition/rendering/ogre2/Ogre2RenderTarget.hh"
#include "ignition/rendering/ogre2/Ogre2RenderTypes.hh"
#include "ignition/rendering/ogre2/Ogre2Scene.hh"
#include "ignition/rendering/ogre2/Ogre2ThermalCamera.hh"
#include "ignition/rendering/ogre2/Ogre2Visual.hh"
#include "ignition/rendering/ogre2/Ogre2WireBox.hh"

#ifdef _MSC_VER
  #pragma warning(push, 0)
#endif
#include <OgreMatrix4.h>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreDepthBuffer.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <Overlay/OgreOverlayManager.h>
#include <Overlay/OgreOverlaySystem.h>
#ifdef _MSC_VER
  #pragma warning(pop)
#endif

/// \brief Private data for the Ogre2Scene class
class ignition::rendering::Ogre2ScenePrivate
{
  /// \brief Flag to indicate if shadows need to be updated
  public: bool shadowsDirty = true;

  /// \brief Flag to indicate if sky is enabled or not
  public: bool skyEnabled = false;

  /// \brief Name of shadow compositor node
  public: const std::string kShadowNodeName = "PbsMaterialsShadowNode";
};

using namespace ignition;
using namespace rendering;

//////////////////////////////////////////////////
Ogre2Scene::Ogre2Scene(unsigned int _id, const std::string &_name) :
  BaseScene(_id, _name), dataPtr(std::make_unique<Ogre2ScenePrivate>())
{
}

//////////////////////////////////////////////////
Ogre2Scene::~Ogre2Scene()
{
}

//////////////////////////////////////////////////
void Ogre2Scene::Fini()
{
}

//////////////////////////////////////////////////
RenderEngine *Ogre2Scene::Engine() const
{
  return Ogre2RenderEngine::Instance();
}

//////////////////////////////////////////////////
VisualPtr Ogre2Scene::RootVisual() const
{
  return this->rootVisual;
}

//////////////////////////////////////////////////
math::Color Ogre2Scene::AmbientLight() const
{
  // This method considers that the ambient upper hemisphere and
  // the lower hemisphere light configurations are the same. For
  // more info check Ogre2Scene::SetAmbientLight documentation.
  Ogre::ColourValue ogreColor =
    this->ogreSceneManager->getAmbientLightUpperHemisphere();
  return Ogre2Conversions::Convert(ogreColor);
}

//////////////////////////////////////////////////
void Ogre2Scene::SetAmbientLight(const math::Color &_color)
{
  // We set the same ambient light for both hemispheres for a
  // traditional fixed-colour ambient light.
  // https://ogrecave.github.io/ogre/api/2.1/class_ogre_1_1_scene
  // _manager.html#a56cd9aa2c4dee4eec9eb07ce1372fb52
  // It's preferred to set the hemisphereDir arg to the up axis,
  // which in our case would be Ogre::Vector3::UNIT_Z
  Ogre::ColourValue ogreColor = Ogre2Conversions::Convert(_color);
  this->ogreSceneManager->setAmbientLight(ogreColor, ogreColor,
      Ogre::Vector3::UNIT_Z);
}


//////////////////////////////////////////////////
void Ogre2Scene::PreRender()
{
  if (this->ShadowsDirty())
  {
    // notify all render targets
    for (unsigned int i  = 0; i < this->SensorCount(); ++i)
    {
      auto camera = std::dynamic_pointer_cast<Camera>(
          this->SensorByIndex(i));
      if (camera)
      {
        // TODO(anyone): this function should rely on virtual functions instead
        // of dynamic casts
        // Looks in commit history for '#SetShadowsNodeDefDirtyABI' to
        // see changes made and revert
        {
          auto cameraDerived = std::dynamic_pointer_cast<Ogre2DepthCamera>(
                                 this->SensorByIndex(i));
          if (cameraDerived)
            cameraDerived->SetShadowsNodeDefDirty();
        }
        {
          auto cameraDerived = std::dynamic_pointer_cast<Ogre2Camera>(
                                 this->SensorByIndex(i));
          if (cameraDerived)
            cameraDerived->SetShadowsNodeDefDirty();
        }
      }
    }

    UpdateShadowNode();
  }

  BaseScene::PreRender();
}

//////////////////////////////////////////////////
void Ogre2Scene::Clear()
{
  this->meshFactory->Clear();

  BaseScene::Clear();
}

//////////////////////////////////////////////////
void Ogre2Scene::Destroy()
{
  this->DestroyNodes();

  // cleanup any items that were not attached to nodes
  // make sure to do this before destroying materials done by BaseScene::Destroy
  // otherwise ogre throws an exception when unlinking a renderable from a
  // hlms datablock
  this->ogreSceneManager->destroyAllItems();

  BaseScene::Destroy();

  if (this->ogreSceneManager)
  {
    this->ogreSceneManager->removeRenderQueueListener(
        Ogre2RenderEngine::Instance()->OverlaySystem());
  }
}

//////////////////////////////////////////////////
Ogre::SceneManager *Ogre2Scene::OgreSceneManager() const
{
  return this->ogreSceneManager;
}

//////////////////////////////////////////////////
bool Ogre2Scene::LoadImpl()
{
  return true;
}

//////////////////////////////////////////////////
bool Ogre2Scene::InitImpl()
{
  this->CreateContext();
  this->CreateRootVisual();
  this->CreateStores();
  this->CreateMeshFactory();

  return true;
}

//////////////////////////////////////////////////
void Ogre2Scene::UpdateShadowNode()
{
  if (!this->ShadowsDirty())
    return;

  unsigned int spotPointLightCount = 0;
  unsigned int dirLightCount = 0;

  for (unsigned int i = 0; i < this->LightCount(); ++i)
  {
    LightPtr light = this->LightByIndex(i);
    if (light->CastShadows())
    {
      if (std::dynamic_pointer_cast<DirectionalLight>(light))
        dirLightCount++;
      else
        spotPointLightCount++;
    }
  }

  // limit number of shadow maps
  // shaders dynamically generated by ogre produce compile error at runtime if
  // the number of shadow maps exceeds certain number. The error seems to
  // suggest that the number of uniform variables has exceeded the max number
  // allowed
  unsigned int maxShadowMaps = 25u;
  if (dirLightCount * 3 + spotPointLightCount > maxShadowMaps)
  {
    dirLightCount = std::min(static_cast<unsigned int>(maxShadowMaps / 3),
        dirLightCount);
    spotPointLightCount = std::min(
        std::max(maxShadowMaps - dirLightCount * 3, 0u), spotPointLightCount);
    ignwarn << "Number of shadow-casting lights exceeds the limit supported by "
            << "the underlying rendering engine ogre2. Limiting to "
            << dirLightCount << " directional lights and "
            << spotPointLightCount << " point / spot lights" << std::endl;
  }

  auto engine = Ogre2RenderEngine::Instance();
  Ogre::CompositorManager2 *compositorManager =
      engine->OgreRoot()->getCompositorManager2();

  Ogre::ShadowNodeHelper::ShadowParamVec shadowParams;
  Ogre::ShadowNodeHelper::ShadowParam shadowParam;

  // directional lights
  unsigned int atlasId = 0u;
  unsigned int texSize = 2048u;
  unsigned int halfTexSize = texSize * 0.5;
  for (unsigned int i = 0; i < dirLightCount; ++i)
  {
    shadowParam.technique = Ogre::SHADOWMAP_PSSM;
    shadowParam.atlasId = atlasId;
    shadowParam.numPssmSplits = 3u;
    shadowParam.resolution[0].x = texSize;
    shadowParam.resolution[0].y = texSize;
    shadowParam.resolution[1].x = halfTexSize;
    shadowParam.resolution[1].y = halfTexSize;
    shadowParam.resolution[2].x = halfTexSize;
    shadowParam.resolution[2].y = halfTexSize;
    shadowParam.atlasStart[0].x = 0u;
    shadowParam.atlasStart[0].y = 0u;
    shadowParam.atlasStart[1].x = 0u;
    shadowParam.atlasStart[1].y = texSize;
    shadowParam.atlasStart[2].x = halfTexSize;
    shadowParam.atlasStart[2].y = texSize;
    shadowParam.supportedLightTypes = 0u;
    shadowParam.addLightType(Ogre::Light::LT_DIRECTIONAL);
    shadowParams.push_back(shadowParam);
    atlasId++;
  }

  // others
  unsigned int maxTexSize = 8192u;
  unsigned int rowIdx = 0;
  unsigned int colIdx = 0;
  unsigned int rowSize = maxTexSize / texSize;
  unsigned int colSize = rowSize;

  for (unsigned int i = 0; i < spotPointLightCount; ++i)
  {
    shadowParam.technique = Ogre::SHADOWMAP_FOCUSED;
    shadowParam.atlasId = atlasId;
    shadowParam.resolution[0].x = texSize;
    shadowParam.resolution[0].y = texSize;
    shadowParam.atlasStart[0].x = colIdx * texSize;
    shadowParam.atlasStart[0].y = rowIdx * texSize;

    shadowParam.supportedLightTypes = 0u;
    shadowParam.addLightType(Ogre::Light::LT_DIRECTIONAL);
    shadowParam.addLightType(Ogre::Light::LT_POINT);
    shadowParam.addLightType(Ogre::Light::LT_SPOTLIGHT);
    shadowParams.push_back(shadowParam);

    colIdx++;
    colIdx = colIdx % colSize;
    if (colIdx == 0u)
      rowIdx++;

    // check if we've filled the current texture atlas
    // if so, increment atlas id to indicate we want a new texture
    if (rowIdx >= rowSize)
    {
      atlasId++;
      colIdx = 0;
      rowIdx = 0;
    }
  }

  std::string shadowNodeDefName = this->dataPtr->kShadowNodeName;
  if (compositorManager->hasShadowNodeDefinition(shadowNodeDefName))
    compositorManager->removeShadowNodeDefinition(shadowNodeDefName);

  this->CreateShadowNodeWithSettings(compositorManager, shadowNodeDefName,
      shadowParams);

  this->SetShadowsDirty(false);
}

////////////////////////////////////////////////////
void Ogre2Scene::CreateShadowNodeWithSettings(
    Ogre::CompositorManager2 *_compositorManager,
    const std::string &_shadowNodeName,
    const Ogre::ShadowNodeHelper::ShadowParamVec &_shadowParams)
{
  Ogre::uint32 pointLightCubemapResolution = 1024u;
  Ogre::Real pssmLambda = 0.95f;
  Ogre::Real splitPadding = 1.0f;
  Ogre::Real splitBlend = 0.125f;
  Ogre::Real splitFade = 0.313f;

  const Ogre::uint32 spotMask           = 1u << Ogre::Light::LT_SPOTLIGHT;
  const Ogre::uint32 directionalMask    = 1u << Ogre::Light::LT_DIRECTIONAL;
  const Ogre::uint32 pointMask          = 1u << Ogre::Light::LT_POINT;
  const Ogre::uint32 spotAndDirMask = spotMask | directionalMask;

  typedef Ogre::vector<Ogre::ShadowNodeHelper::Resolution>::type ResolutionVec;

  size_t numExtraShadowMapsForPssmSplits = 0;
  size_t numTargetPasses = 0;
  ResolutionVec atlasResolutions;

  // Validation and data gathering
  bool hasPointLights = false;

  Ogre::ShadowNodeHelper::ShadowParamVec::const_iterator itor =
      _shadowParams.begin();
  Ogre::ShadowNodeHelper::ShadowParamVec::const_iterator end =
      _shadowParams.end();

  while (itor != end)
  {
    if (itor->technique == Ogre::SHADOWMAP_PSSM)
    {
      numExtraShadowMapsForPssmSplits = itor->numPssmSplits - 1u;
      // 1 per PSSM split
      numTargetPasses += numExtraShadowMapsForPssmSplits + 1u;
    }

    if (itor->atlasId >= atlasResolutions.size())
      atlasResolutions.resize(itor->atlasId + 1u);

    Ogre::ShadowNodeHelper::Resolution &resolution =
        atlasResolutions[itor->atlasId];

    const size_t numSplits = itor->technique == Ogre::SHADOWMAP_PSSM ?
        itor->numPssmSplits : 1u;
    for (size_t i = 0; i < numSplits; ++i)
    {
      resolution.x = std::max(resolution.x,
          itor->atlasStart[i].x + itor->resolution[i].x);
      resolution.y = std::max(resolution.y,
          itor->atlasStart[i].y + itor->resolution[i].y);
    }

    if (itor->supportedLightTypes & pointMask)
    {
      hasPointLights = true;
      // 6 target passes per cubemap + 1 for copy
      numTargetPasses += 7u;
    }
    if (itor->supportedLightTypes & spotAndDirMask &&
        itor->technique != Ogre::SHADOWMAP_PSSM)
    {
      // 1 per directional/spot light (for non-PSSM techniques)
      numTargetPasses += 1u;
    }
    ++itor;
  }

  // One clear for each atlas
  numTargetPasses += atlasResolutions.size();
  // Create the shadow node definition
  Ogre::CompositorShadowNodeDef *shadowNodeDef =
      _compositorManager->addShadowNodeDefinition(_shadowNodeName);

  const size_t numTextures = atlasResolutions.size();
  {
    // Define the atlases (textures)
    shadowNodeDef->setNumLocalTextureDefinitions(
        numTextures + (hasPointLights ? 1u : 0u));
    for (size_t i = 0; i < numTextures; ++i)
    {
      const Ogre::ShadowNodeHelper::Resolution &atlasRes = atlasResolutions[i];
      Ogre::TextureDefinitionBase::TextureDefinition *texDef =
          shadowNodeDef->addTextureDefinition(
          "atlas" + Ogre::StringConverter::toString(i));

      texDef->width = std::max(atlasRes.x, 1u);
      texDef->height = std::max(atlasRes.y, 1u);
      texDef->formatList.push_back(Ogre::PF_D32_FLOAT);
      texDef->depthBufferId = Ogre::DepthBuffer::POOL_NON_SHAREABLE;
      texDef->depthBufferFormat = Ogre::PF_D32_FLOAT;
      texDef->preferDepthTexture = false;
      texDef->fsaa = false;
    }

    // Define the cubemap needed by point lights
    if (hasPointLights)
    {
      Ogre::TextureDefinitionBase::TextureDefinition *texDef =
          shadowNodeDef->addTextureDefinition("tmpCubemap");

      texDef->width   = pointLightCubemapResolution;
      texDef->height  = pointLightCubemapResolution;
      texDef->depth   = 6u;
      texDef->textureType = Ogre::TEX_TYPE_CUBE_MAP;
      texDef->formatList.push_back(Ogre::PF_FLOAT32_R);
      texDef->depthBufferId = 1u;
      texDef->depthBufferFormat = Ogre::PF_D32_FLOAT;
      texDef->preferDepthTexture = false;
      texDef->fsaa = false;
    }
  }

  // Create the shadow maps
  const size_t numShadowMaps =
      _shadowParams.size() + numExtraShadowMapsForPssmSplits;
  shadowNodeDef->setNumShadowTextureDefinitions(numShadowMaps);

  itor = _shadowParams.begin();

  while (itor != end)
  {
    const size_t lightIdx = itor - _shadowParams.begin();
    const Ogre::ShadowNodeHelper::ShadowParam &shadowParam = *itor;

    const Ogre::ShadowNodeHelper::Resolution &texResolution =
        atlasResolutions[shadowParam.atlasId];

    const size_t numSplits =
        shadowParam.technique == Ogre::SHADOWMAP_PSSM ?
        shadowParam.numPssmSplits : 1u;

    for (size_t j = 0; j < numSplits; ++j)
    {
      Ogre::Vector2 uvOffset(
          shadowParam.atlasStart[j].x, shadowParam.atlasStart[j].y);
      Ogre::Vector2 uvLength(
          shadowParam.resolution[j].x, shadowParam.resolution[j].y);

      uvOffset /= Ogre::Vector2(texResolution.x, texResolution.y);
      uvLength /= Ogre::Vector2(texResolution.x, texResolution.y);

      const Ogre::String texName =
          "atlas" + Ogre::StringConverter::toString(shadowParam.atlasId);

      Ogre::ShadowTextureDefinition *shadowTexDef =
          shadowNodeDef->addShadowTextureDefinition(lightIdx, j, texName,
          0, uvOffset, uvLength, 0);
      shadowTexDef->shadowMapTechnique = shadowParam.technique;
      shadowTexDef->pssmLambda = pssmLambda;
      shadowTexDef->splitPadding = splitPadding;
      shadowTexDef->splitBlend = splitBlend;
      shadowTexDef->splitFade = splitFade;
      shadowTexDef->numSplits = numSplits;
    }
    ++itor;
  }

  shadowNodeDef->setNumTargetPass(numTargetPasses);

  // Create the passes for each atlas
  for (size_t atlasId = 0; atlasId < numTextures; ++atlasId)
  {
    const Ogre::String texName =
        "atlas" + Ogre::StringConverter::toString(atlasId);
    {
      // Atlas clear pass
      Ogre::CompositorTargetDef *targetDef =
          shadowNodeDef->addTargetPass(texName);
      targetDef->setNumPasses(1u);

      Ogre::CompositorPassDef *passDef = targetDef->addPass(Ogre::PASS_CLEAR);
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(passDef);
      passClear->mColourValue = Ogre::ColourValue::White;
      passClear->mDepthValue = 1.0f;
    }

    // Pass scene for directional and spot lights first
    size_t shadowMapIdx = 0;
    itor = _shadowParams.begin();
    while (itor != end)
    {
      const Ogre::ShadowNodeHelper::ShadowParam &shadowParam = *itor;
      const size_t numSplits = shadowParam.technique == Ogre::SHADOWMAP_PSSM ?
          shadowParam.numPssmSplits : 1u;
      if (shadowParam.atlasId == atlasId &&
          shadowParam.supportedLightTypes & spotAndDirMask)
      {
        size_t currentShadowMapIdx = shadowMapIdx;
        for (size_t i = 0; i < numSplits; ++i)
        {
          Ogre::CompositorTargetDef *targetDef =
              shadowNodeDef->addTargetPass(texName);
          targetDef->setShadowMapSupportedLightTypes(
              shadowParam.supportedLightTypes & spotAndDirMask);
          targetDef->setNumPasses(1u);

          Ogre::CompositorPassDef *passDef =
              targetDef->addPass(Ogre::PASS_SCENE);
          Ogre::CompositorPassSceneDef *passScene =
              static_cast<Ogre::CompositorPassSceneDef *>(passDef);

          passScene->mShadowMapIdx = currentShadowMapIdx + i;
          passScene->mIncludeOverlays = false;
        }
      }
      shadowMapIdx += numSplits;
      ++itor;
    }

    // Pass scene for point lights last
    shadowMapIdx = 0;
    itor = _shadowParams.begin();
    while (itor != end)
    {
      const Ogre::ShadowNodeHelper::ShadowParam &shadowParam = *itor;
      if (shadowParam.atlasId == atlasId &&
          shadowParam.supportedLightTypes & pointMask)
      {
        // Render to cubemap, each face clear + render
        for (Ogre::uint32 i = 0; i < 6u; ++i)
        {
          Ogre::CompositorTargetDef *targetDef =
              shadowNodeDef->addTargetPass("tmpCubemap", i);
          targetDef->setNumPasses(2u);
          targetDef->setShadowMapSupportedLightTypes(
              shadowParam.supportedLightTypes & pointMask);
          {
            // Clear pass
            Ogre::CompositorPassDef *passDef =
                targetDef->addPass(Ogre::PASS_CLEAR);
            Ogre::CompositorPassClearDef *passClear =
                static_cast<Ogre::CompositorPassClearDef *>(passDef);
            passClear->mColourValue = Ogre::ColourValue::White;
            passClear->mDepthValue = 1.0f;
            passClear->mShadowMapIdx = shadowMapIdx;
          }

          {
            // Scene pass
            Ogre::CompositorPassDef *passDef =
                targetDef->addPass(Ogre::PASS_SCENE);
            Ogre::CompositorPassSceneDef *passScene =
                static_cast<Ogre::CompositorPassSceneDef *>(passDef);
            passScene->mCameraCubemapReorient = true;
            passScene->mShadowMapIdx = shadowMapIdx;
            passScene->mIncludeOverlays = false;
          }
        }

        // Copy to the atlas using a pass quad
        // (Cubemap -> DPSM / Dual Paraboloid).
        Ogre::CompositorTargetDef *targetDef =
            shadowNodeDef->addTargetPass(texName);
        targetDef->setShadowMapSupportedLightTypes(
            shadowParam.supportedLightTypes & pointMask);
        targetDef->setNumPasses(1u);
        Ogre::CompositorPassDef *passDef = targetDef->addPass(Ogre::PASS_QUAD);
        Ogre::CompositorPassQuadDef *passQuad =
            static_cast<Ogre::CompositorPassQuadDef *>(passDef);
        passQuad->mMaterialIsHlms = false;
        passQuad->mMaterialName = "Ogre/DPSM/CubeToDpsm";
        passQuad->addQuadTextureSource(0, "tmpCubemap", 0);
        passQuad->mShadowMapIdx = shadowMapIdx;
      }
      const size_t numSplits = shadowParam.technique ==
          Ogre::SHADOWMAP_PSSM ? shadowParam.numPssmSplits : 1u;
      shadowMapIdx += numSplits;
      ++itor;
    }
  }
}

//////////////////////////////////////////////////
LightStorePtr Ogre2Scene::Lights() const
{
  return this->lights;
}

//////////////////////////////////////////////////
SensorStorePtr Ogre2Scene::Sensors() const
{
  return this->sensors;
}

//////////////////////////////////////////////////
VisualStorePtr Ogre2Scene::Visuals() const
{
  return this->visuals;
}

//////////////////////////////////////////////////
MaterialMapPtr Ogre2Scene::Materials() const
{
  return this->materials;
}

//////////////////////////////////////////////////
DirectionalLightPtr Ogre2Scene::CreateDirectionalLightImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2DirectionalLightPtr light(new Ogre2DirectionalLight);
  bool result = this->InitObject(light, _id, _name);
  return (result) ? light : nullptr;
}

//////////////////////////////////////////////////
PointLightPtr Ogre2Scene::CreatePointLightImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2PointLightPtr light(new Ogre2PointLight);
  bool result = this->InitObject(light, _id, _name);
  return (result) ? light : nullptr;
}

//////////////////////////////////////////////////
SpotLightPtr Ogre2Scene::CreateSpotLightImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2SpotLightPtr light(new Ogre2SpotLight);
  bool result = this->InitObject(light, _id, _name);
  return (result) ? light : nullptr;
}

//////////////////////////////////////////////////
CameraPtr Ogre2Scene::CreateCameraImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2CameraPtr camera(new Ogre2Camera);
  bool result = this->InitObject(camera, _id, _name);
  camera->SetBackgroundColor(this->backgroundColor);
  if (this->backgroundMaterial)
    camera->SetBackgroundMaterial(this->backgroundMaterial);
  return (result) ? camera : nullptr;
}

//////////////////////////////////////////////////
DepthCameraPtr Ogre2Scene::CreateDepthCameraImpl(const unsigned int _id,
    const std::string &_name)
{
  Ogre2DepthCameraPtr camera(new Ogre2DepthCamera);
  bool result = this->InitObject(camera, _id, _name);
  return (result) ? camera : nullptr;
}

//////////////////////////////////////////////////
ThermalCameraPtr Ogre2Scene::CreateThermalCameraImpl(const unsigned int _id,
    const std::string &_name)
{
  Ogre2ThermalCameraPtr camera(new Ogre2ThermalCamera);
  bool result = this->InitObject(camera, _id, _name);
  return (result) ? camera : nullptr;
}

//////////////////////////////////////////////////
GpuRaysPtr Ogre2Scene::CreateGpuRaysImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2GpuRaysPtr gpuRays(new Ogre2GpuRays);
  bool result = this->InitObject(gpuRays, _id, _name);
  return (result) ? gpuRays : nullptr;
}

//////////////////////////////////////////////////
VisualPtr Ogre2Scene::CreateVisualImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2VisualPtr visual(new Ogre2Visual);
  bool result = this->InitObject(visual, _id, _name);
  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
ArrowVisualPtr Ogre2Scene::CreateArrowVisualImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2ArrowVisualPtr visual(new Ogre2ArrowVisual);
  bool result = this->InitObject(visual, _id, _name);
  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
AxisVisualPtr Ogre2Scene::CreateAxisVisualImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2AxisVisualPtr visual(new Ogre2AxisVisual);
  bool result = this->InitObject(visual, _id, _name);
  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
LightVisualPtr Ogre2Scene::CreateLightVisualImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2LightVisualPtr visual(new Ogre2LightVisual);
  bool result = this->InitObject(visual, _id, _name);
  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
GizmoVisualPtr Ogre2Scene::CreateGizmoVisualImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2GizmoVisualPtr visual(new Ogre2GizmoVisual);
  bool result = this->InitObject(visual, _id, _name);
  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
GeometryPtr Ogre2Scene::CreateBoxImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateMeshImpl(_id, _name, "unit_box");
}

//////////////////////////////////////////////////
GeometryPtr Ogre2Scene::CreateConeImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateMeshImpl(_id, _name, "unit_cone");
}

//////////////////////////////////////////////////
GeometryPtr Ogre2Scene::CreateCylinderImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateMeshImpl(_id, _name, "unit_cylinder");
}

//////////////////////////////////////////////////
GeometryPtr Ogre2Scene::CreatePlaneImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateMeshImpl(_id, _name, "unit_plane");
}

//////////////////////////////////////////////////
GeometryPtr Ogre2Scene::CreateSphereImpl(unsigned int _id,
    const std::string &_name)
{
  return this->CreateMeshImpl(_id, _name, "unit_sphere");
}

//////////////////////////////////////////////////
MeshPtr Ogre2Scene::CreateMeshImpl(unsigned int _id, const std::string &_name,
    const std::string &_meshName)
{
  MeshDescriptor descriptor(_meshName);
  return this->CreateMeshImpl(_id, _name, descriptor);
}

//////////////////////////////////////////////////
MeshPtr Ogre2Scene::CreateMeshImpl(unsigned int _id,
    const std::string &_name, const MeshDescriptor &_desc)
{
  Ogre2MeshPtr mesh = this->meshFactory->Create(_desc);
  if (nullptr == mesh)
    return nullptr;

  bool result = this->InitObject(mesh, _id, _name);
  return (result) ? mesh : nullptr;
}

//////////////////////////////////////////////////
CapsulePtr Ogre2Scene::CreateCapsuleImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2CapsulePtr capsule(new Ogre2Capsule);
  bool result = this->InitObject(capsule, _id, _name);
  return (result) ? capsule : nullptr;
}

//////////////////////////////////////////////////
HeightmapPtr Ogre2Scene::CreateHeightmapImpl(unsigned int,
    const std::string &, const HeightmapDescriptor &)
{
  ignerr << "Ogre 2 doesn't support heightmaps yet, see " <<
      "https://github.com/ignitionrobotics/ign-rendering/issues/187"
      << std::endl;
  return nullptr;
}

//////////////////////////////////////////////////
GridPtr Ogre2Scene::CreateGridImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2GridPtr grid(new Ogre2Grid);
  bool result = this->InitObject(grid, _id, _name);
  return (result) ? grid : nullptr;
}

//////////////////////////////////////////////////
WireBoxPtr Ogre2Scene::CreateWireBoxImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2WireBoxPtr wireBox(new Ogre2WireBox);
  bool result = this->InitObject(wireBox, _id, _name);
  return (result) ? wireBox: nullptr;
}

//////////////////////////////////////////////////
MarkerPtr Ogre2Scene::CreateMarkerImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2MarkerPtr marker(new Ogre2Marker);
  bool result = this->InitObject(marker, _id, _name);
  return (result) ? marker: nullptr;
}

//////////////////////////////////////////////////
LidarVisualPtr Ogre2Scene::CreateLidarVisualImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2LidarVisualPtr lidar(new Ogre2LidarVisual);
  bool result = this->InitObject(lidar, _id, _name);
  return (result) ? lidar: nullptr;
}

//////////////////////////////////////////////////
TextPtr Ogre2Scene::CreateTextImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  // TODO(anyone)
  return TextPtr();
}

//////////////////////////////////////////////////
MaterialPtr Ogre2Scene::CreateMaterialImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2MaterialPtr material(new Ogre2Material);
  bool result = this->InitObject(material, _id, _name);
  return (result) ? material : nullptr;
}

//////////////////////////////////////////////////
RenderTexturePtr Ogre2Scene::CreateRenderTextureImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2RenderTexturePtr renderTexture(new Ogre2RenderTexture);
  bool result = this->InitObject(renderTexture, _id, _name);
  return (result) ? renderTexture : nullptr;
}

//////////////////////////////////////////////////
RenderWindowPtr Ogre2Scene::CreateRenderWindowImpl(unsigned int /*_id*/,
    const std::string &/*_name*/)
{
  // TODO(anyone)
  return RenderWindowPtr();
}

//////////////////////////////////////////////////
RayQueryPtr Ogre2Scene::CreateRayQueryImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2RayQueryPtr rayQuery(new Ogre2RayQuery);
  bool result = this->InitObject(rayQuery, _id, _name);
  return (result) ? rayQuery : nullptr;
}

//////////////////////////////////////////////////
ParticleEmitterPtr Ogre2Scene::CreateParticleEmitterImpl(unsigned int _id,
    const std::string &_name)
{
  Ogre2ParticleEmitterPtr visual(new Ogre2ParticleEmitter);
  bool result = this->InitObject(visual, _id, _name);

  return (result) ? visual : nullptr;
}

//////////////////////////////////////////////////
bool Ogre2Scene::InitObject(Ogre2ObjectPtr _object, unsigned int _id,
    const std::string &_name)
{
  // assign needed varibles
  _object->id = _id;
  _object->name = _name;
  _object->scene = this->SharedThis();

  // initialize object
  _object->Load();
  _object->Init();

  return true;
}

//////////////////////////////////////////////////
void Ogre2Scene::CreateContext()
{
  Ogre::Root *root = Ogre2RenderEngine::Instance()->OgreRoot();

  Ogre::InstancingThreadedCullingMethod threadedCullingMethod =
      Ogre::INSTANCING_CULLING_SINGLETHREAD;
  // getNumLogicalCores() may return 0 if couldn't detect
  const size_t numThreads = std::max<size_t>(
      1, Ogre::PlatformInformation::getNumLogicalCores());

  // See ogre doxygen documentation regarding culling methods.
  // In some cases you may still want to use single thread.
  // if( numThreads > 1 )
  //   threadedCullingMethod = Ogre::INSTANCING_CULLING_THREADED;
  // Create the SceneManager, in this case a generic one
  this->ogreSceneManager = root->createSceneManager(Ogre::ST_GENERIC,
                                                    numThreads,
                                                    threadedCullingMethod);

  this->ogreSceneManager->addRenderQueueListener(
      Ogre2RenderEngine::Instance()->OverlaySystem());

  this->ogreSceneManager->getRenderQueue()->setSortRenderQueue(
      Ogre::v1::OverlayManager::getSingleton().mDefaultRenderQueueId,
      Ogre::RenderQueue::StableSort);

  // Set sane defaults for proper shadow mapping
  this->ogreSceneManager->setShadowDirectionalLightExtrusionDistance(500.0f);
  this->ogreSceneManager->setShadowFarDistance(500.0f);

  // enable forward plus to support multiple lights
  // this is required for non-shadow-casting point lights and
  // spot lights to work
  this->ogreSceneManager->setForwardClustered(true, 16, 8, 24, 96, 1, 500);
}

//////////////////////////////////////////////////
void Ogre2Scene::CreateRootVisual()
{
  if (this->rootVisual)
    return;

  // create unregistered visual
  this->rootVisual = Ogre2VisualPtr(new Ogre2Visual);
  unsigned int rootId = this->CreateObjectId();
  std::string rootName = this->CreateObjectName(rootId, "_ROOT_");

  // check if root visual created successfully
  if (!this->InitObject(this->rootVisual, rootId, rootName))
  {
    ignerr << "Unable to create root visual" << std::endl;
    this->rootVisual = nullptr;
    return;
  }

  // add visual node to actual ogre root
  Ogre::SceneNode *ogreRootNode = this->rootVisual->Node();
  this->ogreSceneManager->getRootSceneNode()->addChild(ogreRootNode);
}

//////////////////////////////////////////////////
void Ogre2Scene::CreateMeshFactory()
{
  Ogre2ScenePtr sharedThis = this->SharedThis();
  this->meshFactory = Ogre2MeshFactoryPtr(new Ogre2MeshFactory(sharedThis));
}

//////////////////////////////////////////////////
void Ogre2Scene::CreateStores()
{
  this->lights = Ogre2LightStorePtr(new Ogre2LightStore);
  this->sensors = Ogre2SensorStorePtr(new Ogre2SensorStore);
  this->visuals = Ogre2VisualStorePtr(new Ogre2VisualStore);
  this->materials = Ogre2MaterialMapPtr(new Ogre2MaterialMap);
}

//////////////////////////////////////////////////
Ogre2ScenePtr Ogre2Scene::SharedThis()
{
  ScenePtr sharedBase = this->shared_from_this();
  return std::dynamic_pointer_cast<Ogre2Scene>(sharedBase);
}

//////////////////////////////////////////////////
void Ogre2Scene::SetShadowsDirty(bool _dirty)
{
  this->dataPtr->shadowsDirty = _dirty;
}

//////////////////////////////////////////////////
bool Ogre2Scene::ShadowsDirty() const
{
  return this->dataPtr->shadowsDirty;
}

//////////////////////////////////////////////////
void Ogre2Scene::SetSkyEnabled(bool _enabled)
{
  MaterialPtr skyboxMat;
  if (_enabled)
  {
    // get skybox material
    std::string skyboxMatName = "Default/skybox";
    skyboxMat = this->Material(skyboxMatName);
    if (!skyboxMat)
    {
      // ogre2 should be able to find this texture as resource search
      // paths are already set up in Ogre2RenderEngine.cc
      std::string skyboxEnvMap = "skybox.dds";
      skyboxMat = this->CreateMaterial(skyboxMatName);
      skyboxMat->SetEnvironmentMap(skyboxEnvMap);
    }
  }
  this->SetBackgroundMaterial(skyboxMat);
  for (unsigned int i = 0; i < this->Sensors()->Size(); ++i)
  {
    auto sensor = this->Sensors()->GetByIndex(i);
    auto camera = std::dynamic_pointer_cast<Ogre2Camera>(sensor);
    if (camera)
    {
      camera->SetBackgroundMaterial(skyboxMat);
    }
  }
  this->dataPtr->skyEnabled = _enabled;
}

//////////////////////////////////////////////////
bool Ogre2Scene::SkyEnabled() const
{
  return this->dataPtr->skyEnabled;
}
