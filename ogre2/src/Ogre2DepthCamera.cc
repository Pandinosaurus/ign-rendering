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

#if (_WIN32)
  /* Needed for std::min */
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

#include <math.h>
#include <ignition/math/Helpers.hh>

#include "ignition/rendering/RenderTypes.hh"
#include "ignition/rendering/ogre2/Ogre2Conversions.hh"
#include "ignition/rendering/ogre2/Ogre2DepthCamera.hh"
#include "ignition/rendering/ogre2/Ogre2GaussianNoisePass.hh"
#include "ignition/rendering/ogre2/Ogre2Includes.hh"
#include "ignition/rendering/ogre2/Ogre2ParticleEmitter.hh"
#include "ignition/rendering/ogre2/Ogre2RenderEngine.hh"
#include "ignition/rendering/ogre2/Ogre2RenderTarget.hh"
#include "ignition/rendering/ogre2/Ogre2RenderTypes.hh"
#include "ignition/rendering/ogre2/Ogre2Scene.hh"
#include "ignition/rendering/ogre2/Ogre2Sensor.hh"

#include "Ogre2ParticleNoiseListener.hh"

namespace ignition
{
namespace rendering
{
inline namespace IGNITION_RENDERING_VERSION_NAMESPACE {
//
/// \brief Gaussian noise render pass for depth cameras
/// The class implementation is very similar to Ogre2GaussianNoisePass but
/// uses a different shader material for apply noise to depth cameras
/// This class is added here since we can not modify Ogre2GaussianNoisePass
/// as it would break ABI
class Ogre2DepthGaussianNoisePass : public Ogre2GaussianNoisePass
{
  /// \brief Constructor
  public: Ogre2DepthGaussianNoisePass() {}

  /// \brief Destructor
  public: virtual ~Ogre2DepthGaussianNoisePass() {}

  // Documentation inherited.
  public: void PreRender() override;

  // Documentation inherited.
  public: void CreateRenderPass() override;

  /// brief Pointer to the Gaussian noise ogre material
  private: Ogre::Material *gaussianNoiseMat = nullptr;
};
}
}
}

/// \internal
/// \brief Private data for the Ogre2DepthCamera class
class ignition::rendering::Ogre2DepthCameraPrivate
{
  /// \brief The depth buffer
  public: float *depthBuffer = nullptr;

  /// \brief Outgoing depth data, used by newDepthFrame event.
  public: float *depthImage = nullptr;

  /// \brief Outgoing point cloud data, used by newRgbPointCloud event.
  public: float *pointCloudImage = nullptr;

  /// \brief maximum value used for data outside sensor range
  public: float dataMaxVal = ignition::math::INF_D;

  /// \brief minimum value used for data outside sensor range
  public: float dataMinVal = -ignition::math::INF_D;

  /// \brief 1st pass compositor workspace definition
  public: std::string ogreCompositorWorkspaceDef;

  /// \brief 1st pass compositor node definition
  public: std::string ogreCompositorBaseNodeDef;

  /// \brief Final pass compositor node definition
  public: std::string ogreCompositorFinalNodeDef;

  /// \brief Compositor workspace.
  public: Ogre::CompositorWorkspace *ogreCompositorWorkspace = nullptr;

  /// \brief Output texture with depth and color data
  public: Ogre::TexturePtr ogreDepthTexture[2];

  /// \brief Dummy render texture for the depth data
  public: RenderTexturePtr depthTexture;

  /// \brief The depth material
  public: Ogre::MaterialPtr depthMaterial;

  /// \brief The depth material in final pass
  public: Ogre::MaterialPtr depthFinalMaterial;

  /// \brief A chain of render passes applied to the render target
  public: std::vector<RenderPassPtr> renderPasses;

  /// \brief Flag to indicate if render pass need to be rebuilt
  public: bool renderPassDirty = false;

  /// \brief Event used to signal rgb point cloud data
  public: ignition::common::EventT<void(const float *,
              unsigned int, unsigned int, unsigned int,
              const std::string &)> newRgbPointCloud;

  /// \brief Event used to signal depth data
  public: ignition::common::EventT<void(const float *,
              unsigned int, unsigned int, unsigned int,
              const std::string &)> newDepthFrame;

  /// \brief standard deviation of particle noise
  public: double particleStddev = 0.01;

  /// \brief Listener for setting particle noise value based on particle
  /// emitter region
  public: std::unique_ptr<Ogre2ParticleNoiseListener> particleNoiseListener;

  /// \brief Particle scatter ratio. This is used to determine the ratio of
  /// particles that will detected by the depth camera
  public: double particleScatterRatio = 0.1;

  /// \brief Name of sky box material
  public: const std::string kSkyboxMaterialName = "SkyBox";
};

using namespace ignition;
using namespace rendering;

//////////////////////////////////////////////////
void Ogre2DepthGaussianNoisePass::PreRender()
{
  // This function is similar to Ogre2GaussianNoisePass but duplicated here
  // for Ogre2DepthCamera

  if (!this->gaussianNoiseMat)
    return;

  if (!this->enabled)
    return;

  Ogre::Vector3 offsets(ignition::math::Rand::DblUniform(0.0, 1.0),
                        ignition::math::Rand::DblUniform(0.0, 1.0),
                        ignition::math::Rand::DblUniform(0.0, 1.0));

  Ogre::Pass *pass = this->gaussianNoiseMat->getTechnique(0)->getPass(0);
  Ogre::GpuProgramParametersSharedPtr psParams =
      pass->getFragmentProgramParameters();
  psParams->setNamedConstant("offsets", offsets);
  psParams->setNamedConstant("mean", static_cast<Ogre::Real>(this->mean));
  psParams->setNamedConstant("stddev",
      static_cast<Ogre::Real>(this->stdDev));
}

//////////////////////////////////////////////////
void Ogre2DepthGaussianNoisePass::CreateRenderPass()
{
  // This function is similar to Ogre2GaussianNoisePass but duplicated here
  // for Ogre2DepthCamera.

  static int gaussianDepthNodeCounter = 0;

  auto engine = Ogre2RenderEngine::Instance();
  auto ogreRoot = engine->OgreRoot();
  Ogre::CompositorManager2 *ogreCompMgr = ogreRoot->getCompositorManager2();

  std::string nodeDefName = "GaussianDepthNoiseNode_"
      + std::to_string(gaussianDepthNodeCounter);

  if (ogreCompMgr->hasNodeDefinition(nodeDefName))
    return;

  // The GaussianNoise material is defined in script (gaussian_noise.material).
  // clone the material
  std::string matName = "GaussianNoiseDepth";
  Ogre::MaterialPtr ogreMat =
      Ogre::MaterialManager::getSingleton().getByName(matName);
  if (!ogreMat)
  {
    ignerr << "Gaussian noise material not found: '" << matName << "'"
           << std::endl;
    return;
  }
  if (!ogreMat->isLoaded())
    ogreMat->load();
  std::string materialName = matName + "_" +
      std::to_string(gaussianDepthNodeCounter);
  this->gaussianNoiseMat = ogreMat->clone(materialName).get();

  this->ogreCompositorNodeDefName = nodeDefName;
  gaussianDepthNodeCounter++;

  Ogre::CompositorNodeDef *nodeDef =
      ogreCompMgr->addNodeDefinition(nodeDefName);

  // Input texture
  nodeDef->addTextureSourceName("rt_input", 0,
      Ogre::TextureDefinitionBase::TEXTURE_INPUT);
  nodeDef->addTextureSourceName("rt_output", 1,
      Ogre::TextureDefinitionBase::TEXTURE_INPUT);

  // rt_input target
  nodeDef->setNumTargetPass(1);
  Ogre::CompositorTargetDef *inputTargetDef =
      nodeDef->addTargetPass("rt_output");
  inputTargetDef->setNumPasses(2);
  {
    // clear pass
    inputTargetDef->addPass(Ogre::PASS_CLEAR);

    // quad pass
    Ogre::CompositorPassQuadDef *passQuad =
        static_cast<Ogre::CompositorPassQuadDef *>(
        inputTargetDef->addPass(Ogre::PASS_QUAD));
    passQuad->mMaterialName = materialName;
    passQuad->addQuadTextureSource(0, "rt_input", 0);
  }
  nodeDef->mapOutputChannel(0, "rt_output");
  nodeDef->mapOutputChannel(1, "rt_input");
}

//////////////////////////////////////////////////
Ogre2DepthCamera::Ogre2DepthCamera()
  : dataPtr(new Ogre2DepthCameraPrivate())
{
  this->dataPtr->ogreCompositorWorkspace = nullptr;
}

//////////////////////////////////////////////////
Ogre2DepthCamera::~Ogre2DepthCamera()
{
  this->Destroy();
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::Init()
{
  BaseDepthCamera::Init();

  // create internal camera
  this->CreateCamera();

  // create dummy render texture
  this->CreateRenderTexture();

  this->Reset();
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::Destroy()
{
  if (this->dataPtr->depthBuffer)
  {
    delete [] this->dataPtr->depthBuffer;
    this->dataPtr->depthBuffer = nullptr;
  }

  if (this->dataPtr->depthImage)
  {
    delete [] this->dataPtr->depthImage;
    this->dataPtr->depthImage = nullptr;
  }

  if (this->dataPtr->pointCloudImage)
  {
    delete [] this->dataPtr->pointCloudImage;
    this->dataPtr->pointCloudImage = nullptr;
  }

  if (!this->ogreCamera)
    return;

  auto engine = Ogre2RenderEngine::Instance();
  auto ogreRoot = engine->OgreRoot();
  Ogre::CompositorManager2 *ogreCompMgr = ogreRoot->getCompositorManager2();

  // remove depth texture, material, compositor
  for( size_t i = 0u; i < 2u; ++i )
  {
    if (this->dataPtr->ogreDepthTexture[i])
    {
      Ogre::TextureManager::getSingleton().remove(
            this->dataPtr->ogreDepthTexture[i]->getName());
    }
  }
  if (this->dataPtr->ogreCompositorWorkspace)
  {
    this->RemoveWorkspaceCrashWorkaround();
    ogreCompMgr->removeWorkspace(
        this->dataPtr->ogreCompositorWorkspace);
  }

  if (this->dataPtr->depthMaterial)
  {
    Ogre::MaterialManager::getSingleton().remove(
        this->dataPtr->depthMaterial->getName());
  }

  if (!this->dataPtr->ogreCompositorWorkspaceDef.empty())
  {
    ogreCompMgr->removeWorkspaceDefinition(
        this->dataPtr->ogreCompositorWorkspaceDef);
    ogreCompMgr->removeNodeDefinition(
        this->dataPtr->ogreCompositorBaseNodeDef);
    ogreCompMgr->removeNodeDefinition(
        this->dataPtr->ogreCompositorFinalNodeDef);
  }

  Ogre::SceneManager *ogreSceneManager;
  ogreSceneManager = this->scene->OgreSceneManager();
  if (ogreSceneManager == nullptr)
  {
    ignerr << "Scene manager cannot be obtained" << std::endl;
  }
  else
  {
    if (ogreSceneManager->findCameraNoThrow(this->name) != nullptr)
    {
      ogreSceneManager->destroyCamera(this->ogreCamera);
      this->ogreCamera = nullptr;
    }
  }
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::CreateCamera()
{
  // create ogre camera object
  Ogre::SceneManager *ogreSceneManager;
  ogreSceneManager = this->scene->OgreSceneManager();
  if (ogreSceneManager == nullptr)
  {
    ignerr << "Scene manager cannot be obtained" << std::endl;
    return;
  }

  this->ogreCamera = ogreSceneManager->createCamera(this->name);
  if (this->ogreCamera == nullptr)
  {
    ignerr << "Ogre camera cannot be created" << std::endl;
    return;
  }

  // by default, ogre2 cameras are attached to root scene node
  this->ogreCamera->detachFromParent();
  this->ogreNode->attachObject(this->ogreCamera);

  // rotate to Gazebo coordinate system
  this->ogreCamera->yaw(Ogre::Degree(-90.0));
  this->ogreCamera->roll(Ogre::Degree(-90.0));
  this->ogreCamera->setFixedYawAxis(false);

  // TODO(anyone): provide api access
  this->ogreCamera->setAutoAspectRatio(true);
  this->ogreCamera->setRenderingDistance(100);
  this->ogreCamera->setProjectionType(Ogre::PT_PERSPECTIVE);
  this->ogreCamera->setCustomProjectionMatrix(false);
}

/////////////////////////////////////////////////
void Ogre2DepthCamera::CreateRenderTexture()
{
  RenderTexturePtr base = this->scene->CreateRenderTexture();
  this->dataPtr->depthTexture =
      std::dynamic_pointer_cast<Ogre2RenderTexture>(base);
  this->dataPtr->depthTexture->SetWidth(1);
  this->dataPtr->depthTexture->SetHeight(1);
}

/////////////////////////////////////////////////////////
void Ogre2DepthCamera::CreateDepthTexture()
{
  // set aspect ratio and fov
  double vfov = 2.0 * atan(tan(this->HFOV().Radian() / 2.0) / this->aspect);
  this->ogreCamera->setAspectRatio(this->aspect);
  this->ogreCamera->setFOVy(Ogre::Radian(this->LimitFOV(vfov)));

  // Load depth material
  // The DepthCamera material is defined in script (depth_camera.material).
  // We need to clone it since we are going to modify its uniform variables
  std::string matDepthName = "DepthCamera";
  Ogre::MaterialPtr matDepth =
      Ogre::MaterialManager::getSingleton().getByName(matDepthName);
  this->dataPtr->depthMaterial = matDepth->clone(
      this->Name() + "_" + matDepthName);
  this->dataPtr->depthMaterial->load();
  Ogre::Pass *pass = this->dataPtr->depthMaterial->getTechnique(0)->getPass(0);
  Ogre::GpuProgramParametersSharedPtr psParams =
      pass->getFragmentProgramParameters();

  // Configure camera behaviour.
  // Make the clipping plane dist large and handle near clamping in shaders
  double nearPlane = this->NearClipPlane() * 0.9;
  double farPlane = this->FarClipPlane() * 1.1;
  this->ogreCamera->setNearClipDistance(nearPlane);
  this->ogreCamera->setFarClipDistance(farPlane);

  // Set the uniform variables (depth_camera_fs.glsl).
  // The projectParams is used to linearize depth buffer data
  // The other params are used to clamp the range output
  // Use the 'real' clip distance here so depth can be
  // linearized correctly
  double projectionA = farPlane /
      (farPlane - nearPlane);
  double projectionB = (-farPlane * nearPlane) /
      (farPlane - nearPlane);
  projectionB /= farPlane;
  psParams->setNamedConstant("projectionParams",
      Ogre::Vector2(projectionA, projectionB));
  psParams->setNamedConstant("near",
      static_cast<float>(this->NearClipPlane()));
  psParams->setNamedConstant("far",
      static_cast<float>(this->FarClipPlane()));
  psParams->setNamedConstant("max",
      static_cast<float>(this->dataPtr->dataMaxVal));
  psParams->setNamedConstant("min",
      static_cast<float>(this->dataPtr->dataMinVal));
  Ogre::Vector3 bg(this->Scene()->BackgroundColor().R(),
    this->Scene()->BackgroundColor().G(),
    this->Scene()->BackgroundColor().B());
  psParams->setNamedConstant("backgroundColor", bg);
  psParams->setNamedConstant("particleStddev",
    static_cast<float>(this->dataPtr->particleStddev));

  std::string matDepthFinalName = "DepthCameraFinal";
  Ogre::MaterialPtr matDepthFinal =
      Ogre::MaterialManager::getSingleton().getByName(matDepthFinalName);
  this->dataPtr->depthFinalMaterial = matDepthFinal->clone(
      this->Name() + "_" + matDepthFinalName);
  this->dataPtr->depthFinalMaterial->load();
  Ogre::Pass *passFinal =
      this->dataPtr->depthFinalMaterial->getTechnique(0)->getPass(0);
  Ogre::GpuProgramParametersSharedPtr psParamsFinal =
      passFinal->getFragmentProgramParameters();
  psParamsFinal->setNamedConstant("near",
      static_cast<float>(this->NearClipPlane()));
  psParamsFinal->setNamedConstant("far",
      static_cast<float>(this->FarClipPlane()));
  psParamsFinal->setNamedConstant("max",
      static_cast<float>(this->dataPtr->dataMaxVal));
  psParamsFinal->setNamedConstant("min",
      static_cast<float>(this->dataPtr->dataMinVal));

  // create background material is specified
  MaterialPtr backgroundMaterial = this->Scene()->BackgroundMaterial();
  bool validBackground = backgroundMaterial &&
      !backgroundMaterial->EnvironmentMap().empty();

  if (validBackground)
  {
    Ogre::MaterialManager &matManager = Ogre::MaterialManager::getSingleton();
    std::string skyMatName = this->dataPtr->kSkyboxMaterialName + "_"
        + this->Name();
    auto mat = matManager.getByName(skyMatName);
    if (!mat)
    {
      auto skyboxMat = matManager.getByName(this->dataPtr->kSkyboxMaterialName);
      if (!skyboxMat)
      {
        ignerr << "Unable to find skybox material" << std::endl;
        return;
      }
      mat = skyboxMat->clone(skyMatName);
    }
    Ogre::TextureUnitState *texUnit =
        mat->getTechnique(0u)->getPass(0u)->getTextureUnitState(0u);
    texUnit->setTextureName(backgroundMaterial->EnvironmentMap(),
        Ogre::TEX_TYPE_CUBE_MAP);
  }

  // Create depth camera compositor
  auto engine = Ogre2RenderEngine::Instance();
  auto ogreRoot = engine->OgreRoot();
  Ogre::CompositorManager2 *ogreCompMgr = ogreRoot->getCompositorManager2();

  std::string wsDefName = "DepthCameraWorkspace_" + this->Name();
  this->dataPtr->ogreCompositorWorkspaceDef = wsDefName;
  if (!ogreCompMgr->hasWorkspaceDefinition(wsDefName))
  {
    // The depth camera compositor does a few passes in order to simulate
    // particles effects in depth / point cloud image data
    //
    // render scene (color) with particles, c1
    // render scene (depth) without particles, d1
    // render scene (grayscale) with particles only, g2
    // render scene (depth) with particles only, d2
    //
    //   if g2 is non-zero // pixel with particle
    //     if d2 < d1 // particle is in view
    //       apply noise and scatterbility to d2
    //       set depth data to d2
    //     else
    //       set depth data to d1
    //   set color data to c1

    // We need to programmatically create the compositor because we need to
    // configure it to use the cloned depth material created earlier.
    // The compositor node definition is equivalent to the following:
    //
    // compositor_node DepthCamera
    // {
    //   texture rt0 target_width target_height PF_FLOAT32_RGBA
    //   texture rt1 target_width target_height PF_FLOAT32_RGBA
    //   texture colorTexture target_width target_height PF_R8G8B8
    //       depth_texture depth_format PF_D32_FLOAT
    //   texture depthTexture target_width target_height PF_D32_FLOAT
    //   texture particleTexture target_width target_height PF_L8
    //   // particleDepthTexture shares same depth buffer as particleTexture
    //   texture particleDepthTexture target_width target_height PF_D32_FLOAT
    //   target colorTexture
    //   {
    //     pass clear
    //     {
    //       colour_value 0.0 0.0 0.0 1.0
    //     }
    //     pass render_scene
    //     {
    //     }
    //   }
    //   target depthTexture
    //   {
    //     pass clear
    //     {
    //       colour_value 0.0 0.0 0.0 1.0
    //     }
    //     pass render_scene
    //     {
    //       visibility_mask 0x11011111
    //     }
    //   }
    //   target particleTexture
    //   {
    //     pass clear
    //     {
    //     }
    //     pass render_scene
    //     {
    //       visibility_mask 0x00100000
    //     }
    //   }
    //   target particleDepthTexture
    //   {
    //     pass clear
    //     {
    //     }
    //     pass render_scene
    //     {
    //       visibility_mask 0x00100000
    //     }
    //   }
    //   target rt0
    //   {
    //     pass clear
    //     {
    //     }
    //     pass render_quad
    //     {
    //       material DepthCamera // Use copy instead of original
    //       input 0 depthTexture
    //       input 1 colorTexture
    //       quad_normals camera_far_corners_view_space
    //     }
    //   }
    //   out 0 rt0
    //   out 1 rt1
    // }

    std::string baseNodeDefName = wsDefName + "/BaseNode";
    this->dataPtr->ogreCompositorBaseNodeDef = baseNodeDefName;
    Ogre::CompositorNodeDef *baseNodeDef =
        ogreCompMgr->addNodeDefinition(baseNodeDefName);

    baseNodeDef->addTextureSourceName(
          "rt0", 0u, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    baseNodeDef->addTextureSourceName(
          "rt1", 1u, Ogre::TextureDefinitionBase::TEXTURE_INPUT);

    Ogre::TextureDefinitionBase::TextureDefinition *depthTexDef =
        baseNodeDef->addTextureDefinition("depthTexture");
    depthTexDef->textureType = Ogre::TEX_TYPE_2D;
    depthTexDef->width = 0;
    depthTexDef->height = 0;
    depthTexDef->depth = 1;
    depthTexDef->numMipmaps = 0;
    depthTexDef->widthFactor = 1;
    depthTexDef->heightFactor = 1;
    depthTexDef->formatList = {Ogre::PF_D32_FLOAT};
    depthTexDef->fsaa = 0;
    depthTexDef->uav = false;
    depthTexDef->automipmaps = false;
    depthTexDef->hwGammaWrite = Ogre::TextureDefinitionBase::BoolFalse;
    depthTexDef->depthBufferId = Ogre::DepthBuffer::POOL_DEFAULT;
    depthTexDef->depthBufferFormat = Ogre::PF_UNKNOWN;
    depthTexDef->fsaaExplicitResolve = false;

    Ogre::TextureDefinitionBase::TextureDefinition *colorTexDef =
        baseNodeDef->addTextureDefinition("colorTexture");
    colorTexDef->textureType = Ogre::TEX_TYPE_2D;
    colorTexDef->width = 0;
    colorTexDef->height = 0;
    colorTexDef->depth = 1;
    colorTexDef->numMipmaps = 0;
    colorTexDef->widthFactor = 1;
    colorTexDef->heightFactor = 1;
    colorTexDef->formatList = {Ogre::PF_R8G8B8};
    colorTexDef->fsaa = 0;
    colorTexDef->uav = false;
    colorTexDef->automipmaps = false;
    // Enable gamma write to avoid discretization in the color values
    // Note we are using low level materials in quad pass so also had to perform
    // gamma correction in the fragment shaders (depth_camera_fs.glsl)
    colorTexDef->hwGammaWrite = Ogre::TextureDefinitionBase::BoolTrue;
    colorTexDef->depthBufferId = Ogre::DepthBuffer::POOL_DEFAULT;
    colorTexDef->depthBufferFormat = Ogre::PF_D32_FLOAT;
    colorTexDef->preferDepthTexture = true;
    colorTexDef->fsaaExplicitResolve = false;

    Ogre::TextureDefinitionBase::TextureDefinition *particleTexDef =
        baseNodeDef->addTextureDefinition("particleTexture");
    particleTexDef->textureType = Ogre::TEX_TYPE_2D;
    particleTexDef->width = 0;
    particleTexDef->height = 0;
    particleTexDef->depth = 1;
    particleTexDef->numMipmaps = 0;
    particleTexDef->widthFactor = 0.5;
    particleTexDef->heightFactor = 0.5;
    particleTexDef->formatList = {Ogre::PF_L8};
    particleTexDef->fsaa = 0;
    particleTexDef->uav = false;
    particleTexDef->automipmaps = false;
    particleTexDef->hwGammaWrite = Ogre::TextureDefinitionBase::BoolFalse;
    particleTexDef->depthBufferId = Ogre::DepthBuffer::POOL_DEFAULT;
    particleTexDef->depthBufferFormat = Ogre::PF_UNKNOWN;
    particleTexDef->preferDepthTexture = false;
    particleTexDef->fsaaExplicitResolve = false;

    Ogre::TextureDefinitionBase::TextureDefinition *particleDepthTexDef =
        baseNodeDef->addTextureDefinition("particleDepthTexture");
    particleDepthTexDef->textureType = Ogre::TEX_TYPE_2D;
    particleDepthTexDef->width = 0;
    particleDepthTexDef->height = 0;
    particleDepthTexDef->depth = 1;
    particleDepthTexDef->numMipmaps = 0;
    particleDepthTexDef->widthFactor = 0.5;
    particleDepthTexDef->heightFactor = 0.5;
    particleDepthTexDef->formatList = {Ogre::PF_D32_FLOAT};
    particleDepthTexDef->fsaa = 0;
    particleDepthTexDef->uav = false;
    particleDepthTexDef->automipmaps = false;
    particleDepthTexDef->hwGammaWrite = Ogre::TextureDefinitionBase::BoolFalse;
    particleDepthTexDef->depthBufferId = Ogre::DepthBuffer::POOL_NON_SHAREABLE;
    particleDepthTexDef->depthBufferFormat = Ogre::PF_UNKNOWN;
    particleDepthTexDef->fsaaExplicitResolve = false;

    baseNodeDef->setNumTargetPass(5);

    Ogre::CompositorTargetDef *colorTargetDef =
        baseNodeDef->addTargetPass("colorTexture");

    if (validBackground)
      colorTargetDef->setNumPasses(3);
    else
      colorTargetDef->setNumPasses(2);
    {
      // clear pass
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(
          colorTargetDef->addPass(Ogre::PASS_CLEAR));
      passClear->mColourValue = Ogre::ColourValue(
          Ogre2Conversions::Convert(this->Scene()->BackgroundColor()));

      if (validBackground)
      {
        // quad pass
        Ogre::CompositorPassQuadDef *passQuad =
            static_cast<Ogre::CompositorPassQuadDef *>(
            colorTargetDef->addPass(Ogre::PASS_QUAD));
        passQuad->mMaterialName = this->dataPtr->kSkyboxMaterialName + "_"
            + this->Name();
        passQuad->mFrustumCorners =
            Ogre::CompositorPassQuadDef::CAMERA_DIRECTION;
      }

      // scene pass
      Ogre::CompositorPassSceneDef *passScene =
          static_cast<Ogre::CompositorPassSceneDef *>(
          colorTargetDef->addPass(Ogre::PASS_SCENE));
      passScene->mVisibilityMask = IGN_VISIBILITY_ALL;

      // todo(anyone) PbsMaterialsShadowNode is hardcoded.
      // Although this may be just fine
      passScene->mShadowNode = "PbsMaterialsShadowNode";
    }

    Ogre::CompositorTargetDef *depthTargetDef =
        baseNodeDef->addTargetPass("depthTexture");
    depthTargetDef->setNumPasses(2);
    {
      // clear pass
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(
          depthTargetDef->addPass(Ogre::PASS_CLEAR));
      passClear->mColourValue = Ogre::ColourValue(this->FarClipPlane(),
          this->FarClipPlane(), this->FarClipPlane());

      // scene pass
      Ogre::CompositorPassSceneDef *passScene =
          static_cast<Ogre::CompositorPassSceneDef *>(
          depthTargetDef->addPass(Ogre::PASS_SCENE));
      // depth texute does not contain particles
      passScene->mVisibilityMask = IGN_VISIBILITY_ALL
          & ~Ogre2ParticleEmitter::kParticleVisibilityFlags;
    }

    Ogre::CompositorTargetDef *particleTargetDef =
        baseNodeDef->addTargetPass("particleTexture");
    particleTargetDef->setNumPasses(2);
    {
      // clear pass
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(
          particleTargetDef->addPass(Ogre::PASS_CLEAR));
      passClear->mColourValue = Ogre::ColourValue::Black;

      // scene pass
      Ogre::CompositorPassSceneDef *passScene =
          static_cast<Ogre::CompositorPassSceneDef *>(
          particleTargetDef->addPass(Ogre::PASS_SCENE));
      passScene->mVisibilityMask =
          Ogre2ParticleEmitter::kParticleVisibilityFlags;
    }

    Ogre::CompositorTargetDef *particleDepthTargetDef =
        baseNodeDef->addTargetPass("particleDepthTexture");
    particleDepthTargetDef->setNumPasses(2);
    {
      // clear pass
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(
          particleDepthTargetDef->addPass(Ogre::PASS_CLEAR));
      passClear->mColourValue = Ogre::ColourValue(this->FarClipPlane(),
          this->FarClipPlane(), this->FarClipPlane());

      // scene pass
      Ogre::CompositorPassSceneDef *passScene =
          static_cast<Ogre::CompositorPassSceneDef *>(
          particleDepthTargetDef->addPass(Ogre::PASS_SCENE));
      passScene->mVisibilityMask =
          Ogre2ParticleEmitter::kParticleVisibilityFlags;
    }

    // rt0 target - converts depth to xyz
    Ogre::CompositorTargetDef *inTargetDef =
        baseNodeDef->addTargetPass("rt0");
    inTargetDef->setNumPasses(2);
    {
      // clear pass
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(
          inTargetDef->addPass(Ogre::PASS_CLEAR));
      passClear->mColourValue = Ogre::ColourValue(this->FarClipPlane(),
          this->FarClipPlane(), this->FarClipPlane());

      // quad pass
      Ogre::CompositorPassQuadDef *passQuad =
          static_cast<Ogre::CompositorPassQuadDef *>(
          inTargetDef->addPass(Ogre::PASS_QUAD));
      passQuad->mMaterialName = this->dataPtr->depthMaterial->getName();
      passQuad->addQuadTextureSource(0, "depthTexture", 0);
      passQuad->addQuadTextureSource(1, "colorTexture", 0);
      passQuad->addQuadTextureSource(2, "particleTexture", 0);
      passQuad->addQuadTextureSource(3, "particleDepthTexture", 0);
      passQuad->mFrustumCorners =
          Ogre::CompositorPassQuadDef::VIEW_SPACE_CORNERS;
    }

    baseNodeDef->mapOutputChannel(0, "rt0");
    baseNodeDef->mapOutputChannel(1, "rt1");

    // Programmatically create the final pass node and use the cloned final
    // depth material created earlier.
    // The compositor node definition is equivalent to the following:
    //
    // compositor_node DepthCameraFinal
    // {
    //   in 0 rt_output
    //   in 1 rt_input
    //
    //   target rt_output
    //   {
    //     pass clear
    //     {
    //     }
    //     pass render_quad
    //     {
    //       material DepthCameraFinal // Use copy instead of original
    //       input 0 rt_input
    //     }
    //   }
    // }

    std::string finalNodeDefName = wsDefName + "/FinalNode";
    this->dataPtr->ogreCompositorFinalNodeDef = finalNodeDefName;
    Ogre::CompositorNodeDef *finalNodeDef =
        ogreCompMgr->addNodeDefinition(finalNodeDefName);

    finalNodeDef->addTextureSourceName("rt_input", 0,
        Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    // output texture
    finalNodeDef->addTextureSourceName("rt_output", 1,
        Ogre::TextureDefinitionBase::TEXTURE_INPUT);

    finalNodeDef->setNumTargetPass(1);
    // rt_output target - converts depth to xyz
    Ogre::CompositorTargetDef *outputTargetDef =
        finalNodeDef->addTargetPass("rt_output");
    outputTargetDef->setNumPasses(2);
    {
      // clear pass
      Ogre::CompositorPassClearDef *passClear =
          static_cast<Ogre::CompositorPassClearDef *>(
          outputTargetDef->addPass(Ogre::PASS_CLEAR));
      passClear->mColourValue = Ogre::ColourValue(this->FarClipPlane(),
          this->FarClipPlane(), this->FarClipPlane());

      // quad pass
      Ogre::CompositorPassQuadDef *passQuad =
          static_cast<Ogre::CompositorPassQuadDef *>(
          outputTargetDef->addPass(Ogre::PASS_QUAD));
      passQuad->mMaterialName = this->dataPtr->depthFinalMaterial->getName();
      passQuad->addQuadTextureSource(0, "rt_input", 0);
    }
    finalNodeDef->mapOutputChannel(0, "rt_output");

    // Finally create the workspace.
    // The compositor workspace definition is equivalent to the following:
    //
    // workspace DepthCameraWorkspace
    // {
    //   connect_output DepthCameraFinal 0
    //   connect DepthCamera 0 DepthCameraFinal 1
    // }
    Ogre::CompositorWorkspaceDef *workDef =
        ogreCompMgr->addWorkspaceDefinition(wsDefName);

    workDef->connectExternal(0, baseNodeDefName, 0);
    workDef->connectExternal(1, baseNodeDefName, 1);
    workDef->connect(baseNodeDefName, finalNodeDefName);
  }
  Ogre::CompositorWorkspaceDef *wsDef =
      ogreCompMgr->getWorkspaceDefinition(wsDefName);

  if (!wsDef)
  {
    ignerr << "Unable to add workspace definition [" << wsDefName << "] "
           << " for " << this->Name();
  }

  // create render texture - these textures pack the range data
  for( size_t i = 0u; i < 2u; ++i )
  {
    this->dataPtr->ogreDepthTexture[i] =
      Ogre::TextureManager::getSingleton().createManual(
      this->Name() + "_depth" + std::to_string(i), "General",
      Ogre::TEX_TYPE_2D, this->ImageWidth(), this->ImageHeight(), 1, 0,
      Ogre::PF_FLOAT32_RGBA, Ogre::TU_RENDERTARGET,
      0, false, 0, Ogre::BLANKSTRING, false, true);

    Ogre::RenderTarget *rt =
        this->dataPtr->ogreDepthTexture[i]->getBuffer()->getRenderTarget();
    rt->setDepthBufferPool(Ogre::DepthBuffer::POOL_INVALID);
  }

  CreateWorkspaceInstance();
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::CreateWorkspaceInstance()
{
  auto engine = Ogre2RenderEngine::Instance();
  auto ogreRoot = engine->OgreRoot();
  Ogre::CompositorManager2 *ogreCompMgr = ogreRoot->getCompositorManager2();

  Ogre::CompositorChannelVec externalTargets(2u);
  for( size_t i = 0u; i < 2u; ++i )
  {
    externalTargets[i].target =
        this->dataPtr->ogreDepthTexture[i]->getBuffer()->getRenderTarget();
    externalTargets[i].textures.push_back(this->dataPtr->ogreDepthTexture[i]);
  }

  // create compositor worksspace
  this->dataPtr->ogreCompositorWorkspace =
      ogreCompMgr->addWorkspace(this->scene->OgreSceneManager(),
      externalTargets, this->ogreCamera,
      this->dataPtr->ogreCompositorWorkspaceDef, false);

  // add the listener
  Ogre::CompositorNode *node =
      this->dataPtr->ogreCompositorWorkspace->getNodeSequence()[0];
  auto channelsTex = node->getLocalTextures();

  for (auto c : channelsTex)
  {
    if (c.textures[0]->getSrcFormat() == Ogre::PF_L8)
    {
      // add particle noise / scatter effects listener so we can set the
      // amount of noise based on size of emitter
      this->dataPtr->particleNoiseListener.reset(
          new Ogre2ParticleNoiseListener(this->scene,
          this->ogreCamera, this->dataPtr->depthMaterial));
      c.target->addListener(this->dataPtr->particleNoiseListener.get());
      break;
    }
  }
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::Render()
{
  // update the compositors
  this->dataPtr->ogreCompositorWorkspace->setEnabled(true);
  auto engine = Ogre2RenderEngine::Instance();
  engine->OgreRoot()->renderOneFrame();
  this->dataPtr->ogreCompositorWorkspace->setEnabled(false);
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::PreRender()
{
  if (!this->dataPtr->ogreDepthTexture[0])
    this->CreateDepthTexture();

  if (!this->dataPtr->ogreCompositorWorkspace)
    this->CreateWorkspaceInstance();

  Ogre::Texture *rawDepthTextures[2] =
  {
    this->dataPtr->ogreDepthTexture[0].get(),
    this->dataPtr->ogreDepthTexture[1].get()
  };

  // update depth camera render passes
  Ogre2RenderTarget::UpdateRenderPassChain(
      this->dataPtr->ogreCompositorWorkspace,
      this->dataPtr->ogreCompositorWorkspaceDef,
      this->dataPtr->ogreCompositorBaseNodeDef,
      this->dataPtr->ogreCompositorFinalNodeDef,
      this->dataPtr->renderPasses,
      this->dataPtr->renderPassDirty,
      &rawDepthTextures,
      false);

  if (rawDepthTextures[0] != this->dataPtr->ogreDepthTexture[0].get())
  {
    std::swap( this->dataPtr->ogreDepthTexture[0],
               this->dataPtr->ogreDepthTexture[1] );
  }

  for (auto &pass : this->dataPtr->renderPasses)
    pass->PreRender();


  // add the particle noise listener again if worksapce is recreated due to
  // dirty render pass
  if (this->dataPtr->renderPassDirty)
  {
    Ogre::CompositorNode *node =
        this->dataPtr->ogreCompositorWorkspace->getNodeSequence()[0];
    auto channelsTex = node->getLocalTextures();

    for (auto c : channelsTex)
    {
      if (c.textures[0]->getSrcFormat() == Ogre::PF_L8)
      {
        // add particle noise / scatter effects listener so we can set the
        // amount of noise based on size of emitter
        this->dataPtr->particleNoiseListener.reset(
            new Ogre2ParticleNoiseListener(this->scene,
            this->ogreCamera, this->dataPtr->depthMaterial));
        c.target->addListener(this->dataPtr->particleNoiseListener.get());
        break;
      }
    }
  }

  this->dataPtr->renderPassDirty = false;
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::PostRender()
{
  unsigned int width = this->ImageWidth();
  unsigned int height = this->ImageHeight();

  PixelFormat format = PF_FLOAT32_RGBA;
  Ogre::PixelFormat imageFormat = Ogre2Conversions::Convert(format);

  size_t size = Ogre::PixelUtil::getMemorySize(width, height, 1, imageFormat);
  int len = width * height;
  unsigned int channelCount = PixelUtil::ChannelCount(format);

  if (!this->dataPtr->depthBuffer)
  {
    this->dataPtr->depthBuffer = new float[len * channelCount];
  }
  Ogre::PixelBox dstBox(width, height,
        1, imageFormat, this->dataPtr->depthBuffer);

  // blit data from gpu to cpu
  auto rt = this->dataPtr->ogreDepthTexture[1]->getBuffer()->getRenderTarget();
  rt->copyContentsToMemory(dstBox, Ogre::RenderTarget::FB_AUTO);

  if (!this->dataPtr->depthImage)
  {
    this->dataPtr->depthImage = new float[len];
  }
  if (!this->dataPtr->pointCloudImage)
  {
    this->dataPtr->pointCloudImage = new float[len * channelCount];
  }

  // fill depth data
  for (unsigned int i = 0; i < height; ++i)
  {
    unsigned int step = i*width*channelCount;
    for (unsigned int j = 0; j < width; ++j)
    {
      float x = this->dataPtr->depthBuffer[step + j*channelCount];
      this->dataPtr->depthImage[i*width + j] = x;
    }
  }
  this->dataPtr->newDepthFrame(
        this->dataPtr->depthImage, width, height, 1, "FLOAT32");

  // point cloud data
  if (this->dataPtr->newRgbPointCloud.ConnectionCount() > 0u)
  {
    memcpy(this->dataPtr->pointCloudImage, this->dataPtr->depthBuffer, size);
    this->dataPtr->newRgbPointCloud(
        this->dataPtr->pointCloudImage, width, height, channelCount,
        "PF_FLOAT32_RGBA");

    // Uncomment to debug color output
    // for (unsigned int i = 0; i < height; ++i)
    // {
    //   unsigned int step = i*width*channelCount;
    //   for (unsigned int j = 0; j < width; ++j)
    //   {
    //     float color =
    //         this->dataPtr->pointCloudImage[step + j*channelCount + 3];
    //     // unpack rgb data
    //     uint32_t *rgba = reinterpret_cast<uint32_t *>(&color);
    //     unsigned int r = *rgba >> 24 & 0xFF;
    //     unsigned int g = *rgba >> 16 & 0xFF;
    //     unsigned int b = *rgba >> 8 & 0xFF;
    //     igndbg << "[" << r << "]" << "[" << g << "]" << "[" << b << "],";
    //   }
    //   igndbg << std::endl;
    // }

    // Uncomment to debug xyz output
    // igndbg << "wxh: " << width << " x " << height << std::endl;
    // for (unsigned int i = 0; i < height; ++i)
    // {
    //   for (unsigned int j = 0; j < width; ++j)
    //   {
    //     igndbg << "[" << this->dataPtr->pointCloudImage[i*width*4+j*4] << "]"
    //       << "[" << this->dataPtr->pointCloudImage[i*width*4+j*4+1] << "]"
    //       << "[" << this->dataPtr->pointCloudImage[i*width*4+j*4+2] << "],";
    //   }
    //   igndbg << std::endl;
    // }
  }

  // Uncomment to debug depth output
  // igndbg << "wxh: " << width << " x " << height << std::endl;
  // for (unsigned int i = 0; i < height; ++i)
  // {
  //   for (unsigned int j = 0; j < width; ++j)
  //   {
  //     igndbg << "[" << this->dataPtr->depthImage[i*width + j] << "]";
  //   }
  //   igndbg << std::endl;
  // }
}

//////////////////////////////////////////////////
const float *Ogre2DepthCamera::DepthData() const
{
  return this->dataPtr->depthBuffer;
}

//////////////////////////////////////////////////
ignition::common::ConnectionPtr Ogre2DepthCamera::ConnectNewDepthFrame(
    std::function<void(const float *, unsigned int, unsigned int,
      unsigned int, const std::string &)>  _subscriber)
{
  return this->dataPtr->newDepthFrame.Connect(_subscriber);
}

//////////////////////////////////////////////////
ignition::common::ConnectionPtr Ogre2DepthCamera::ConnectNewRgbPointCloud(
    std::function<void(const float *, unsigned int, unsigned int,
      unsigned int, const std::string &)>  _subscriber)
{
  return this->dataPtr->newRgbPointCloud.Connect(_subscriber);
}

//////////////////////////////////////////////////
RenderTargetPtr Ogre2DepthCamera::RenderTarget() const
{
  return this->dataPtr->depthTexture;
}

//////////////////////////////////////////////////
double Ogre2DepthCamera::LimitFOV(const double _fov)
{
  return std::min(std::max(0.001, _fov), IGN_PI * 0.999);
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::SetNearClipPlane(const double _near)
{
  BaseDepthCamera::SetNearClipPlane(_near);
  // near plane clipping is handled in shaders
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::SetFarClipPlane(const double _far)
{
  BaseDepthCamera::SetFarClipPlane(_far);
  // far plane clipping is handled in shaders
}

//////////////////////////////////////////////////
double Ogre2DepthCamera::NearClipPlane() const
{
  return BaseDepthCamera::NearClipPlane();
}

//////////////////////////////////////////////////
double Ogre2DepthCamera::FarClipPlane() const
{
  return BaseDepthCamera::FarClipPlane();
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::SetShadowsNodeDefDirty()
{
  if (!this->dataPtr->ogreCompositorWorkspace)
    return;

  auto engine = Ogre2RenderEngine::Instance();
  auto ogreRoot = engine->OgreRoot();
  Ogre::CompositorManager2 *ogreCompMgr = ogreRoot->getCompositorManager2();

  this->RemoveWorkspaceCrashWorkaround();
  ogreCompMgr->removeWorkspace( this->dataPtr->ogreCompositorWorkspace );
  this->dataPtr->ogreCompositorWorkspace = nullptr;
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::RemoveWorkspaceCrashWorkaround()
{
  Ogre::MaterialPtr material =
      Ogre::MaterialManager::getSingleton().
      getByName (this->dataPtr->depthMaterial->getName());

  if (!material.isNull())
  {
    for (size_t i = 0; i < 4; ++i)
    {
      material->getBestTechnique()->getPass(0)->
          getTextureUnitState(i)->setBlank();
    }
  }
}

//////////////////////////////////////////////////
void Ogre2DepthCamera::AddRenderPass(const RenderPassPtr &_pass)
{
  // hack: check and only allow gaussian noise for depth cameras
  // We create a new depth gaussion noise render pass object
  // (class declared in this src file) so that we can change the shader material
  // to use for applying noise to depth data.
  // The proper solution would be to either add a new DepthGaussianNoisePass
  // class or extend the Ogre2GaussianNoisePass to handle both color and
  // depth cameras
  std::shared_ptr<Ogre2GaussianNoisePass> pass =
      std::dynamic_pointer_cast<Ogre2GaussianNoisePass>(_pass);
  if (!pass)
  {
    ignerr << "Depth camera currently only supports a gaussian noise pass"
           << std::endl;
    return;
  }

  // create new depth noise pass
  std::shared_ptr<Ogre2DepthGaussianNoisePass> depthNoisePass =
    std::make_shared<Ogre2DepthGaussianNoisePass>();
  depthNoisePass->SetMean(pass->Mean());
  depthNoisePass->SetStdDev(pass->StdDev());

  this->dataPtr->renderPasses.push_back(depthNoisePass);
  this->dataPtr->renderPassDirty = true;
}
