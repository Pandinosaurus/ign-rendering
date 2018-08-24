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
#ifndef IGNITION_RENDERING_OGRE2_OGRE2VISUAL_HH_
#define IGNITION_RENDERING_OGRE2_OGRE2VISUAL_HH_

#include "ignition/rendering/base/BaseVisual.hh"
#include "ignition/rendering/ogre2/Ogre2Node.hh"
#include "ignition/rendering/ogre2/Ogre2RenderTypes.hh"

namespace ignition
{
  namespace rendering
  {
    inline namespace IGNITION_RENDERING_VERSION_NAMESPACE {
    //
    /// \brief Ogre2.x implementation of the visual class
    class IGNITION_RENDERING_OGRE2_VISIBLE Ogre2Visual :
      public BaseVisual<Ogre2Node>
    {
      /// \brief Constructor
      protected: Ogre2Visual();

      /// \brief Destructor
      public: virtual ~Ogre2Visual();

      // Documentation inherited.
      public: virtual math::Vector3d LocalScale() const override;

      // Documentation inherited.
      public: virtual bool InheritScale() const override;

      // Documentation inherited.
      public: virtual void SetInheritScale(bool _inherit) override;

      // Documentation inherited.
      protected: virtual NodeStorePtr Children() const override;

      // Documentation inherited.
      protected: virtual GeometryStorePtr Geometries() const override;

      // Documentation inherited.
      protected: virtual bool AttachChild(NodePtr _child) override;

      // Documentation inherited.
      protected: virtual bool DetachChild(NodePtr _child) override;

      // Documentation inherited.
      protected: virtual bool AttachGeometry(GeometryPtr _geometry) override;

      // Documentation inherited.
      protected: virtual bool DetachGeometry(GeometryPtr _geometry) override;

      /// \brief Implementation of the SetLocalScale function
      /// \param[in] _scale Scale to set the visual to
      protected: virtual void SetLocalScaleImpl(
                     const math::Vector3d &_scale);

      /// \brief Initialize the visual
      protected: virtual void Init();

      /// \brief Get a shared pointer to this.
      /// \return Shared pointer to this
      private: Ogre2VisualPtr SharedThis();

      /// \brief Pointer to the child nodes
      protected: Ogre2NodeStorePtr children;

      /// \brief Pointer to the attached geometries
      protected: Ogre2GeometryStorePtr geometries;

      /// \brief Make scene our friend so it can create ogre2 visuals
      private: friend class Ogre2Scene;
    };
    }
  }
}
#endif
