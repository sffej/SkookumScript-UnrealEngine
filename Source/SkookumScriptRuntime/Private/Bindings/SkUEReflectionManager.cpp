//=======================================================================================
// Copyright (c) 2001-2017 Agog Labs Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//=======================================================================================

//=======================================================================================
// SkookumScript Plugin for Unreal Engine 4
//
// Class for interfacing with UE4 Blueprint graphs 
//=======================================================================================

#include "SkUEReflectionManager.hpp"
#include "VectorMath/SkVector2.hpp"
#include "VectorMath/SkVector3.hpp"
#include "VectorMath/SkVector4.hpp"
#include "VectorMath/SkRotationAngles.hpp"
#include "VectorMath/SkTransform.hpp"
#include "Engine/SkUEEntity.hpp"
#include "Engine/SkUEActor.hpp"
#include "SkUEUtils.hpp"
#include "SkookumScriptInstanceProperty.h"
#include "../../../SkookumScriptGenerator/Private/SkookumScriptGeneratorBase.h"

#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Engine/UserDefinedStruct.h"

#include <SkookumScript/SkExpressionBase.hpp>
#include <SkookumScript/SkInvokedCoroutine.hpp>
#include <SkookumScript/SkParameterBase.hpp>
#include <SkookumScript/SkBoolean.hpp>
#include <SkookumScript/SkEnum.hpp>
#include <SkookumScript/SkInteger.hpp>
#include <SkookumScript/SkReal.hpp>

//---------------------------------------------------------------------------------------

SkUEReflectionManager * SkUEReflectionManager::ms_singleton_p;

UScriptStruct * SkUEReflectionManager::ms_struct_vector2_p;
UScriptStruct * SkUEReflectionManager::ms_struct_vector3_p;
UScriptStruct * SkUEReflectionManager::ms_struct_vector4_p;
UScriptStruct * SkUEReflectionManager::ms_struct_rotation_angles_p;
UScriptStruct * SkUEReflectionManager::ms_struct_transform_p;

//---------------------------------------------------------------------------------------

SkUEReflectionManager::SkUEReflectionManager()
  {
  SK_ASSERTX(!ms_singleton_p, "There can be only one instance of this class.");
  ms_singleton_p = this;

  ms_struct_vector2_p          = FindObjectChecked<UScriptStruct>(UObject::StaticClass()->GetOutermost(), TEXT("Vector2D"), false);
  ms_struct_vector3_p          = FindObjectChecked<UScriptStruct>(UObject::StaticClass()->GetOutermost(), TEXT("Vector"), false);
  ms_struct_vector4_p          = FindObjectChecked<UScriptStruct>(UObject::StaticClass()->GetOutermost(), TEXT("Vector4"), false);
  ms_struct_rotation_angles_p  = FindObjectChecked<UScriptStruct>(UObject::StaticClass()->GetOutermost(), TEXT("Rotator"), false);
  ms_struct_transform_p        = FindObjectChecked<UScriptStruct>(UObject::StaticClass()->GetOutermost(), TEXT("Transform"), false);

  m_result_name = ASymbol::create("result");

  // Get package to attach reflected classes to
  m_module_package_p = FindObject<UPackage>(nullptr, TEXT("/Script/SkookumScriptRuntime"));
  SK_ASSERTX(m_module_package_p, "SkookumScriptRuntime module package not found!");
  if (!m_module_package_p)
    {
    m_module_package_p = GetTransientPackage();
    }
  }

//---------------------------------------------------------------------------------------

SkUEReflectionManager::~SkUEReflectionManager()
  {
  clear(nullptr);

  SK_ASSERTX_NO_THROW(ms_singleton_p == this, "There can be only one instance of this class.");
  ms_singleton_p = nullptr;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::clear(tSkUEOnFunctionRemovedFromClassFunc * on_function_removed_from_class_f)
  {
  // Destroy all UFunctions and UProperties we allocated
  for (uint32_t i = 0; i < m_reflected_functions.get_length(); ++i)
    {
    delete_reflected_function(i);
    }

  // And forget pointers to them
  m_reflected_functions.empty();

  // Clear out references in classes
  for (ReflectedClass * reflected_class_p : m_reflected_classes)
    {
    #if WITH_EDITOR
      // Invoke callback for each affected class
      if (!reflected_class_p->m_functions.is_empty() && on_function_removed_from_class_f && reflected_class_p->m_ue_static_class_p.IsValid())
        {
        on_function_removed_from_class_f->invoke(reflected_class_p->m_ue_static_class_p.Get());
        }
    #endif

    reflected_class_p->m_functions.empty();
    }
  }

//---------------------------------------------------------------------------------------
// Build list of all &blueprint annotated routines, but do not bind them to UE4 yet
bool SkUEReflectionManager::sync_all_from_sk(tSkUEOnFunctionRemovedFromClassFunc * on_function_removed_from_class_f)
  {
  // Mark all bindings for delete
  for (ReflectedFunction * reflected_function_p : m_reflected_functions)
    {
    if (reflected_function_p)
      {
      reflected_function_p->m_marked_for_delete_all = true;
      }
    }

  // Traverse Sk classes and gather methods that want to be exposed
  bool anything_changed = sync_class_from_sk_recursively(SkUEEntity::get_class(), on_function_removed_from_class_f);

  // Now go and delete anything still marked for delete
  for (ReflectedClass * reflected_class_p : m_reflected_classes)
    {
    bool removed_function_from_class = false;
    for (uint32_t i = 0; i < reflected_class_p->m_functions.get_length(); ++i)
      {      
      uint32_t function_index = reflected_class_p->m_functions[i].m_idx;
      ReflectedFunction * reflected_function_p = m_reflected_functions[function_index];
      if (reflected_function_p && reflected_function_p->m_marked_for_delete_all)
        {
        delete_reflected_function(function_index);
        reflected_class_p->m_functions.remove_fast(i--);
        removed_function_from_class = true;
        anything_changed = true;
        }
      }

    #if WITH_EDITOR
      // Invoke callback for each affected class
      if (removed_function_from_class && on_function_removed_from_class_f && reflected_class_p->m_ue_static_class_p.IsValid())
        {
        on_function_removed_from_class_f->invoke(reflected_class_p->m_ue_static_class_p.Get());
        }
    #endif
    }

  return anything_changed;
  }

//---------------------------------------------------------------------------------------
// Bind all routines in the binding list to UE4 by generating UFunction objects
bool SkUEReflectionManager::sync_class_from_sk(SkClass * sk_class_p, tSkUEOnFunctionRemovedFromClassFunc * on_function_removed_from_class_f)
  {
  // Find existing methods of this class and mark them for delete
  ReflectedClass * reflected_class_p = m_reflected_classes.get(sk_class_p->get_name());
  if (reflected_class_p)
    {
    for (FunctionIndex function_index : reflected_class_p->m_functions)
      {
      ReflectedFunction * reflected_function_p = m_reflected_functions[function_index.m_idx];
      if (reflected_function_p)
        {
        reflected_function_p->m_marked_for_delete_class = true;
        }
      }
    }

  // Make sure reflected classes exist for all classes that need to store an SkInstance
  if ((!reflected_class_p || !reflected_class_p->m_store_sk_instance)
   && does_class_need_instance_property(sk_class_p) 
   && !does_class_need_instance_property(sk_class_p->get_superclass()))
    {
    if (!reflected_class_p)
      {
      reflected_class_p = new ReflectedClass(sk_class_p->get_name());
      m_reflected_classes.append(*reflected_class_p);
      }
    reflected_class_p->m_store_sk_instance = true;
    }

  // Gather new functions/events
  int32_t change_count = 0;
  for (auto method_p : sk_class_p->get_instance_methods())
    {
    change_count += (int32_t)try_add_reflected_function(method_p);
    }
  for (auto method_p : sk_class_p->get_class_methods())
    {
    change_count += (int32_t)try_add_reflected_function(method_p);
    }
  for (auto coroutine_p : sk_class_p->get_coroutines())
    {
    change_count += (int32_t)try_add_reflected_function(coroutine_p);
    }

  // Now go and delete anything still marked for delete
  uint32_t delete_count = 0;
  if (reflected_class_p)
    {
    for (uint32_t i = 0; i < reflected_class_p->m_functions.get_length(); ++i)
      {
      uint32_t function_index = reflected_class_p->m_functions[i].m_idx;
      ReflectedFunction * reflected_function_p = m_reflected_functions[function_index];
      if (reflected_function_p && reflected_function_p->m_marked_for_delete_class)
        {
        delete_reflected_function(function_index);
        reflected_class_p->m_functions.remove_fast(i--);
        ++delete_count;
        }
      }

    // Notify caller
    #if WITH_EDITOR
      if (delete_count && on_function_removed_from_class_f && reflected_class_p->m_ue_static_class_p.IsValid())
        {
        on_function_removed_from_class_f->invoke(reflected_class_p->m_ue_static_class_p.Get());
        }
    #endif
    }

  return (change_count + delete_count > 0);
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::sync_class_from_sk_recursively(SkClass * sk_class_p, tSkUEOnFunctionRemovedFromClassFunc * on_function_removed_from_class_f)
  {
  // Sync this class
  bool anything_changed = sync_class_from_sk(sk_class_p, on_function_removed_from_class_f);

  // Gather sub classes
  for (SkClass * sk_subclass_p : sk_class_p->get_subclasses())
    {
    anything_changed |= sync_class_from_sk_recursively(sk_subclass_p, on_function_removed_from_class_f);
    }

  return anything_changed;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::try_add_reflected_function(SkInvokableBase * sk_invokable_p)
  {
  if (sk_invokable_p->get_annotation_flags() & SkAnnotation_ue4_blueprint)
    {
    // If it's a method with no body...
    if (sk_invokable_p->get_invoke_type() == SkInvokable_method_func
     || sk_invokable_p->get_invoke_type() == SkInvokable_method_mthd)
      { // ...it's an event
      return add_reflected_event(static_cast<SkMethodBase *>(sk_invokable_p));
      }
    else if (sk_invokable_p->get_invoke_type() == SkInvokable_method
          || sk_invokable_p->get_invoke_type() == SkInvokable_coroutine)
      { // ...otherwise it's a function/coroutine
      return add_reflected_call(sk_invokable_p);
      }
    else
      {
      SK_ERRORX(a_str_format("Trying to export coroutine %s to Blueprints which is atomic. Currently only scripted coroutines can be invoked via Blueprints.", sk_invokable_p->get_name_cstr()));
      }
    }
  else if (sk_invokable_p->get_scope()->get_annotation_flags() & SkAnnotation_reflected_data)
    {
    // If it's a method with no body inside a Blueprint generated class...
    if (sk_invokable_p->get_invoke_type() == SkInvokable_method_func
     || sk_invokable_p->get_invoke_type() == SkInvokable_method_mthd)
      { // ...it's a Blueprint function or custom event
      return add_reflected_event(static_cast<SkMethodBase *>(sk_invokable_p));
      }    
    }

  // Nothing changed
  return false;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::try_update_reflected_function(SkInvokableBase * sk_invokable_p, ReflectedClass ** out_reflected_class_pp, int32_t * out_function_index_p)
  {
  SK_ASSERTX(out_function_index_p, "Must be non-null");

  const tSkParamList & param_list = sk_invokable_p->get_params().get_param_list();

  // See if we find any compatible entry already present:  
  ReflectedClass * reflected_class_p = m_reflected_classes.get(sk_invokable_p->get_scope()->get_name());
  if (reflected_class_p)
    {
    *out_reflected_class_pp = reflected_class_p;

    for (FunctionIndex function_index : reflected_class_p->m_functions)
      {
      ReflectedFunction * reflected_function_p = m_reflected_functions[function_index.m_idx];
      if (reflected_function_p
       && reflected_function_p->get_name()        == sk_invokable_p->get_name()
       && reflected_function_p->m_is_class_member == sk_invokable_p->is_class_member())
        {
        // There is no overloading in SkookumScript
        // Therefore if the above matches we found our slot
        *out_function_index_p = function_index.m_idx;

        // Can't update if signatures don't match
        if (reflected_function_p->m_num_params != param_list.get_length()
         || reflected_function_p->m_result_type.m_sk_class_name != sk_invokable_p->get_params().get_result_class()->get_key_class()->get_name())
          {
          return false;
          }
        if (reflected_function_p->m_type == ReflectedFunctionType_Call)
          {
          ReflectedCall * reflected_call_p = static_cast<ReflectedCall *>(reflected_function_p);
          if (!have_identical_signatures(param_list, reflected_call_p->get_param_array()))
            {
            return false;
            }

          // Re-resolve pointers to parameter types to make sure they point to the correct SkClass objects
          rebind_params_to_sk(param_list, reflected_call_p->get_param_array());
          // Re-resolve result type too
          reflected_call_p->m_result_type.m_sk_class_p = sk_invokable_p->get_params().get_result_class()->get_key_class();
          }
        else
          {
          ReflectedEvent * reflected_event_p = static_cast<ReflectedEvent *>(reflected_function_p);
          if (!have_identical_signatures(param_list, reflected_event_p->get_param_array()))
            {
            return false;
            }

          // Re-resolve pointers to parameter types to make sure they point to the correct SkClass objects
          rebind_params_to_sk(param_list, reflected_event_p->get_param_array());
          // For events, remember which binding index to invoke...
          sk_invokable_p->set_user_data(function_index.m_idx);
          // ...and which atomic function to use
          bind_event_method(static_cast<SkMethodBase *>(sk_invokable_p));
          }

        // We're good to update
        reflected_function_p->m_sk_invokable_p = sk_invokable_p; // Update Sk method pointer
        reflected_function_p->m_marked_for_delete_class = false; // Keep around
        reflected_function_p->m_marked_for_delete_all = false; // Keep around
        return true; // Successfully updated
        }
      }
    }
  else
    {
    *out_reflected_class_pp = nullptr;
    }

  // No matching entry found at all
  *out_function_index_p = -1;
  return false;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::add_reflected_call(SkInvokableBase * sk_invokable_p)
  {
  // Check if this reflected call already exists, and if so, just update it
  ReflectedClass * reflected_class_p;
  int32_t function_index;
  if (try_update_reflected_function(sk_invokable_p, &reflected_class_p, &function_index))
    {
    return false; // Nothing changed
    }
  if (function_index >= 0)
    {
    delete_reflected_function(function_index);
    reflected_class_p->m_functions.remove(FunctionIndex(function_index));
    }

  // Parameters of the method we are creating
  const SkParameters & params = sk_invokable_p->get_params();
  const tSkParamList & param_list = params.get_param_list();
  uint32_t num_params = param_list.get_length();

  // Allocate reflected call
  ReflectedCall * reflected_call_p = new(FMemory::Malloc(sizeof(ReflectedCall) + num_params * sizeof(ReflectedCallParam))) ReflectedCall(sk_invokable_p, num_params, params.get_result_class()->get_key_class());

  // Initialize parameters
  for (uint32_t i = 0; i < num_params; ++i)
    {
    const SkParameterBase * input_param = param_list[i];
    new (&reflected_call_p->get_param_array()[i]) ReflectedCallParam(input_param->get_name(), input_param->get_expected_type()->get_key_class());
    }

  // Store reflected call in array
  store_reflected_function(reflected_call_p, reflected_class_p, function_index);

  // This entry changed
  return true;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::add_reflected_event(SkMethodBase * sk_method_p)
  {
  // Check if this reflected event already exists, and if so, just update it
  ReflectedClass * reflected_class_p;
  int32_t function_index;
  if (try_update_reflected_function(sk_method_p, &reflected_class_p, &function_index))
    {
    return false; // Nothing changed
    }
  if (function_index >= 0)
    {
    delete_reflected_function(function_index);
    reflected_class_p->m_functions.remove(FunctionIndex(function_index));
    }

  // Parameters of the method we are creating
  const SkParameters & params = sk_method_p->get_params();
  const tSkParamList & param_list = params.get_param_list();
  uint32_t num_params = param_list.get_length();

  // Bind Sk method
  bind_event_method(sk_method_p);

  // Allocate reflected event
  ReflectedEvent * reflected_event_p = new(FMemory::Malloc(sizeof(ReflectedEvent) + num_params * sizeof(ReflectedEventParam))) ReflectedEvent(sk_method_p, num_params, params.get_result_class()->get_key_class());

  // Initialize parameters
  for (uint32_t i = 0; i < num_params; ++i)
    {
    const SkParameterBase * input_param_p = param_list[i];
    new (&reflected_event_p->get_param_array()[i]) ReflectedEventParam(input_param_p->get_name(), input_param_p->get_expected_type()->get_key_class());
    }

  // Store reflected event in array
  store_reflected_function(reflected_event_p, reflected_class_p, function_index);

  // This entry changed
  return true;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::expose_reflected_function(uint32_t function_index, tSkUEOnFunctionUpdatedFunc * on_function_updated_f, bool is_final)
  {
  bool anything_changed = false;

  ReflectedFunction * reflected_function_p = m_reflected_functions[function_index];
  if (reflected_function_p && reflected_function_p->m_sk_invokable_p)
    {
    // Only expose entries that have not already been exposed
    if (!reflected_function_p->m_ue_function_p.IsValid())
      {
      // Find reflected class belonging to this reflected function - must exist at this point
      ReflectedClass * reflected_class_p = m_reflected_classes.get(reflected_function_p->m_sk_invokable_p->get_scope()->get_name());
      // Get or look up UE4 equivalent of the class
      UClass * ue_static_class_p = reflected_class_p->m_ue_static_class_p.Get();
      if (!ue_static_class_p)
        {
        reflected_class_p->m_ue_static_class_p = ue_static_class_p = SkUEClassBindingHelper::get_static_ue_class_from_sk_class_super(reflected_function_p->m_sk_invokable_p->get_scope());
        }
     if (ue_static_class_p)
        {
        anything_changed = true;

        // Allocate ReflectedPropertys to store temporary information
        ReflectedProperty * param_info_array_p = a_stack_allocate(reflected_function_p->m_num_params + 1, ReflectedProperty);
        for (uint32_t i = 0; i < reflected_function_p->m_num_params + 1u; ++i)
          {
          new (param_info_array_p + i) ReflectedProperty();
          }

        // Now build UFunction
        UFunction * ue_function_p;
        if (reflected_function_p->m_type == ReflectedFunctionType_Event 
         && (reflected_function_p->m_sk_invokable_p->get_scope()->get_annotation_flags() & SkAnnotation_reflected_data)
         && !(reflected_function_p->m_sk_invokable_p->get_annotation_flags() & SkAnnotation_ue4_blueprint))
          {
          // It's a Blueprint function or a custom event, look it up
          ue_function_p = reflect_ue_function(reflected_function_p->m_sk_invokable_p, param_info_array_p, is_final);
          }
        else
          {
          // If function not there yet, build it
          ue_function_p = build_ue_function(ue_static_class_p, reflected_function_p->m_sk_invokable_p, reflected_function_p->m_type, function_index, param_info_array_p, is_final);
          }

        // Fill in the parameter information
        if (ue_function_p)
          {
          reflected_function_p->m_ue_function_p = ue_function_p;

          const ReflectedProperty & return_info = param_info_array_p[reflected_function_p->m_num_params];
          reflected_function_p->m_result_type.m_byte_size = return_info.m_ue_property_p ? return_info.m_ue_property_p->GetSize() : 0;

          if (reflected_function_p->m_type == ReflectedFunctionType_Call)
            {
            ReflectedCall * reflected_call_p = static_cast<ReflectedCall *>(reflected_function_p);

            // Initialize parameters
            for (uint32_t i = 0; i < reflected_function_p->m_num_params; ++i)
              {
              const ReflectedProperty & param_info = param_info_array_p[i];
              ReflectedCallParam & param_entry = reflected_call_p->get_param_array()[i];
              param_entry.m_byte_size = param_info.m_ue_property_p->GetSize();
              param_entry.m_fetcher_p = param_info.m_k2_param_fetcher_p;
              }

            // And return parameter
            reflected_call_p->m_result_getter = return_info.m_sk_value_storer_p;
            }
          else
            {
            ReflectedEvent * reflected_event_p = static_cast<ReflectedEvent *>(reflected_function_p);

            // Initialize parameters
            for (uint32_t i = 0; i < reflected_function_p->m_num_params; ++i)
              {
              const ReflectedProperty & param_info = param_info_array_p[i];
              ReflectedEventParam & param_entry = reflected_event_p->get_param_array()[i];
              param_entry.m_byte_size = param_info.m_ue_property_p->GetSize();
              param_entry.m_storer_p = param_info.m_sk_value_storer_p;
              param_entry.m_assigner_p = param_info.m_ue_property_p->HasAllPropertyFlags(CPF_OutParm) ? param_info.m_k2_value_assigner_p : nullptr;
              param_entry.m_offset = param_info.m_ue_property_p->GetOffset_ForUFunction();
              }

            // And return parameter
            reflected_event_p->m_result_getter = return_info.m_k2_value_fetcher_p;
            }

          // Clear parent class function cache if exists
          // as otherwise it might have cached a nullptr which might cause it to never find newly added functions
          #if WITH_EDITORONLY_DATA
            UClass * ue_class_p = SkUEClassBindingHelper::get_ue_class_from_sk_class(reflected_function_p->m_sk_invokable_p->get_scope());
            if (ue_class_p)
              {
              ue_class_p->ClearFunctionMapsCaches();
              }
          #endif

          // Invoke update callback if any
          if (on_function_updated_f)
            {
            on_function_updated_f->invoke(ue_function_p, reflected_function_p->m_type == ReflectedFunctionType_Event);
            }
          }

        // And destroy the ReflectedPropertys
        for (uint32_t i = 0; i < reflected_function_p->m_num_params + 1u; ++i)
          {
          param_info_array_p[i].~ReflectedProperty();
          }
        }
      }
    }

  return anything_changed;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::sync_all_to_ue(tSkUEOnFunctionUpdatedFunc * on_function_updated_f, bool is_final)
  {
  bool anything_changed = false;

  // Loop through all the reflected classes and attach a USkookumScriptInstanceProperty as needed
  for (ReflectedClass * reflected_class_p : m_reflected_classes)
    {
    if (reflected_class_p->m_store_sk_instance)
      {
      SkClass * sk_class_p = SkBrain::get_class(reflected_class_p->get_name());
      if (sk_class_p && !sk_class_p->get_user_data_int())
        {
        UClass * ue_class_p = SkUEClassBindingHelper::get_ue_class_from_sk_class(sk_class_p);
        if (ue_class_p)
          {
          reflected_class_p->m_ue_static_class_p = ue_class_p;
          anything_changed |= add_instance_property_to_class(ue_class_p, sk_class_p);
          }
        }
      }
    }

  // Loop through all bindings and generate their UFunctions
  for (uint32_t binding_index = 0; binding_index < m_reflected_functions.get_length(); ++binding_index)
    {
    anything_changed |= expose_reflected_function(binding_index, on_function_updated_f, is_final);
    }

  return anything_changed;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::does_class_need_instance_property(SkClass * sk_class_p)
  {
  return (sk_class_p->get_annotation_flags() & SkAnnotation_reflected_data)
      && (sk_class_p->get_total_data_count() || sk_class_p->find_instance_method(ASymbolX_ctor) || sk_class_p->find_instance_method(ASymbolX_dtor));
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::add_instance_property_to_class(UClass * ue_class_p, SkClass * sk_class_p)
  {
  bool success = false;

  // Name it like the class for simplicity
  FName property_name = USkookumScriptInstanceProperty::StaticClass()->GetFName();

  // Is it already present (in this class or any of its superclasses) ?
  UProperty * property_p = ue_class_p->FindPropertyByName(property_name);
  if (!property_p)
    {
    // No objects of this class except the CDO must exist yet
    #if defined(MAD_CHECK) && (SKOOKUM & SK_DEBUG)
      TArray<UObject *> objects;
      GetObjectsOfClass(ue_class_p, objects);
      SK_ASSERTX(objects.Num() == 0, a_str_format("%d objects of class '%S' already exist when its USkookumScriptInstanceProperty is attached!", objects.Num(), *ue_class_p->GetName()));
    #endif

    // Attach new property
    property_p = NewObject<USkookumScriptInstanceProperty>(ue_class_p, property_name);
    // Note: The CDO was already created and _does not_ have this property!
    // So: Append to the end of the children's chain where it won't shift other properties around in memory
    // And, to prevent problems with the smaller CDO, all code in USkookumScriptInstanceProperty interacting with CDOs simply does nothing
    UField ** prev_pp = &ue_class_p->Children;
    while (*prev_pp) { prev_pp = &(*prev_pp)->Next; }
    *prev_pp = property_p;
    // Relink special pointers
    ue_class_p->StaticLink(true);

    // Something changed!
    success = true;
    }

  // Remember offset in the object where the SkInstance pointer is stored
  sk_class_p->set_user_data_int_recursively(property_p->GetOffset_ForInternal());

  return success;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::can_ue_property_be_reflected(UProperty * ue_property_p)
  {
  return reflect_ue_property(ue_property_p);
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::is_skookum_reflected_call(UFunction * function_p)
  {
  Native native_function_p = function_p->GetNativeFunc();
  return native_function_p == (Native)&SkUEReflectionManager::exec_class_method
      || native_function_p == (Native)&SkUEReflectionManager::exec_instance_method
      || native_function_p == (Native)&SkUEReflectionManager::exec_coroutine;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::is_skookum_reflected_event(UFunction * function_p)
  {
  return function_p->RepOffset == EventMagicRepOffset;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::exec_method(FFrame & stack, void * const result_p, SkClass * class_scope_p, SkInstance * this_p)
  {
  const ReflectedCall & reflected_call = static_cast<const ReflectedCall &>(*ms_singleton_p->m_reflected_functions[stack.CurrentNativeFunction->RepOffset]);
  SK_ASSERTX(reflected_call.m_type == ReflectedFunctionType_Call, "ReflectedFunction has bad type!");
  SK_ASSERTX(reflected_call.m_sk_invokable_p->get_invoke_type() == SkInvokable_method, "Must be a method at this point.");

  SkMethodBase * method_p = static_cast<SkMethodBase *>(reflected_call.m_sk_invokable_p);
  if (method_p->get_scope() != class_scope_p)
    {
    method_p = static_cast<SkMethodBase *>(class_scope_p->get_invokable_from_vtable(this_p ? SkScope_instance : SkScope_class, method_p->get_vtable_index()));
    #if SKOOKUM & SK_DEBUG
      // If not found, might be due to recent live update and the vtable not being updated yet - try finding it by name
      if (!method_p || method_p->get_name() != reflected_call.get_name())
        {
        method_p = this_p
          ? class_scope_p->find_instance_method_inherited(reflected_call.get_name())
          : class_scope_p->find_class_method_inherited(reflected_call.get_name());
        }
      // If still not found, that means the method placed in the graph is not in a parent class of class_scope_p
      if (!method_p)
        {
        // Just revert to original method and then, after processing the arguments on the stack, assert below
        method_p = static_cast<SkMethodBase *>(reflected_call.m_sk_invokable_p);
        }
    #endif
    }
  SkInvokedMethod imethod(nullptr, this_p, method_p, a_stack_allocate(method_p->get_invoked_data_array_size(), SkInstance*));

  SKDEBUG_ICALL_SET_INTERNAL(&imethod);
  SKDEBUG_HOOK_SCRIPT_ENTRY(reflected_call.get_name());

  // Fill invoked method's argument list
  const ReflectedCallParam * call_params_p = reflected_call.get_param_array();
  SK_ASSERTX(imethod.get_data().get_size() >= reflected_call.m_num_params, a_str_format("Not enough space (%d) for %d arguments while invoking '%s@%s'!", imethod.get_data().get_size(), reflected_call.m_num_params, reflected_call.m_sk_invokable_p->get_scope()->get_name_cstr_dbg(), reflected_call.get_name_cstr_dbg()));
  for (uint32_t i = 0; i < reflected_call.m_num_params; ++i)
    {
    const ReflectedCallParam & call_param = call_params_p[i];
    imethod.data_append_arg((*call_param.m_fetcher_p)(stack, call_param));
    }

  // Done with stack - now increment the code ptr unless it is null
  stack.Code += !!stack.Code;

  #if (SKOOKUM & SK_DEBUG)
    if (!class_scope_p->is_class(*reflected_call.m_sk_invokable_p->get_scope()))
      {
      SK_ERRORX(a_str_format("Attempted to invoke method '%s@%s' via a blueprint of type '%s'. You might have forgotten to specify the SkookumScript type of this blueprint as '%s' in its SkookumScriptClassDataComponent.", reflected_call.m_sk_invokable_p->get_scope()->get_name_cstr(), reflected_call.get_name_cstr(), this_p->get_class()->get_name_cstr(), reflected_call.m_sk_invokable_p->get_scope()->get_name_cstr()));
      }
    else
  #endif
      {
      // Call method
      SkInstance * result_instance_p = SkBrain::ms_nil_p;
      static_cast<SkMethod *>(method_p)->SkMethod::invoke(&imethod, nullptr, &result_instance_p); // We know it's a method so call directly
      // And pass back the result
      if (reflected_call.m_result_getter)
        {
        (*reflected_call.m_result_getter)(result_p, result_instance_p, reflected_call.m_result_type);
        }
      result_instance_p->dereference();
      }

  SKDEBUG_HOOK_SCRIPT_EXIT();
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::exec_class_method(FFrame & stack, void * const result_p)
  {
  SkClass * class_scope_p = SkUEClassBindingHelper::get_object_class((UObject *)this);
  exec_method(stack, result_p, class_scope_p, nullptr);
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::exec_instance_method(FFrame & stack, void * const result_p)
  {
  SkInstance * this_p = SkUEEntity::new_instance((UObject *)this);
  exec_method(stack, result_p, this_p->get_class(), this_p);
  this_p->dereference();
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::exec_coroutine(FFrame & stack, void * const result_p)
  {
  const ReflectedCall & reflected_call = static_cast<const ReflectedCall &>(*ms_singleton_p->m_reflected_functions[stack.CurrentNativeFunction->RepOffset]);
  SK_ASSERTX(reflected_call.m_type == ReflectedFunctionType_Call, "ReflectedFunction has bad type!");
  SK_ASSERTX(reflected_call.m_sk_invokable_p->get_invoke_type() == SkInvokable_coroutine, "Must be a coroutine at this point.");

  // Get instance of this object
  SkInstance * this_p = SkUEEntity::new_instance((UObject *)this);

  // Create invoked coroutine
  SkCoroutineBase * coro_p = static_cast<SkCoroutineBase *>(reflected_call.m_sk_invokable_p);
  SkClass * class_scope_p = this_p->get_class();
  if (coro_p->get_scope() != class_scope_p)
    {
    coro_p = static_cast<SkCoroutine *>(class_scope_p->get_invokable_from_vtable_i(coro_p->get_vtable_index()));
    #if SKOOKUM & SK_DEBUG
      // If not found, might be due to recent live update and the vtable not being updated yet - try finding it by name
      if (coro_p == nullptr || coro_p->get_name() != reflected_call.m_sk_invokable_p->get_name())
        {
        coro_p = class_scope_p->find_coroutine_inherited(reflected_call.m_sk_invokable_p->get_name());
        }
      // If still not found, that means the coroutine placed in the graph is not in a parent class of class_scope_p
      if (!coro_p)
        {
        // Just revert to original coroutine and then, after processing the arguments on the stack, assert below
        coro_p = static_cast<SkCoroutineBase *>(reflected_call.m_sk_invokable_p);
        }
    #endif
    }
  SkInvokedCoroutine * icoroutine_p = SkInvokedCoroutine::pool_new(coro_p);

  // Set parameters
  icoroutine_p->reset(SkCall_interval_always, nullptr, this_p, nullptr, nullptr);

  #if defined(SKDEBUG_COMMON)
    // Set with SKDEBUG_ICALL_STORE_GEXPR stored here before calls to argument expressions
    // overwrite it.
    const SkExpressionBase * call_expr_p = SkInvokedContextBase::ms_last_expr_p;
  #endif

  SKDEBUG_ICALL_SET_EXPR(icoroutine_p, call_expr_p);

  // Fill invoked coroutine's argument list
  const ReflectedCallParam * param_entry_array = reflected_call.get_param_array();
  icoroutine_p->data_ensure_size(reflected_call.m_num_params);
  for (uint32_t i = 0; i < reflected_call.m_num_params; ++i)
    {
    const ReflectedCallParam & param_entry = param_entry_array[i];
    icoroutine_p->data_append_arg((*param_entry.m_fetcher_p)(stack, param_entry));
    }

  // Done with stack - now increment the code ptr unless it is null
  stack.Code += !!stack.Code;

  SKDEBUG_HOOK_EXPR(call_expr_p, icoroutine_p, nullptr, nullptr, SkDebug::HookContext_peek);

  #if (SKOOKUM & SK_DEBUG)
    if (!this_p->get_class()->is_class(*reflected_call.m_sk_invokable_p->get_scope()))
      {
      SK_ERRORX(a_str_format("Attempted to invoke coroutine '%s@%s' via a blueprint of type '%s'. You might have forgotten to specify the SkookumScript type of this blueprint as '%s' in its SkookumScriptClassDataComponent.", reflected_call.m_sk_invokable_p->get_scope()->get_name_cstr(), reflected_call.get_name_cstr(), this_p->get_class()->get_name_cstr(), reflected_call.m_sk_invokable_p->get_scope()->get_name_cstr()));
      }
    else
  #endif
      {
      // Invoke the coroutine on this_p - might return immediately
      icoroutine_p->on_update();
      }

  // Free if not in use by our invoked coroutine
  this_p->dereference();
  }

//---------------------------------------------------------------------------------------
// Execute a blueprint event
void SkUEReflectionManager::mthd_trigger_event(SkInvokedMethod * scope_p, SkInstance ** result_pp)
  {
  uint32_t function_index = scope_p->get_invokable()->get_user_data();
  const ReflectedEvent & reflected_event = static_cast<const ReflectedEvent &>(*ms_singleton_p->m_reflected_functions[function_index]);
  SK_ASSERTX(reflected_event.m_type == ReflectedFunctionType_Event, "ReflectedFunction has bad type!");

  // Create parameters on stack
  const ReflectedEventParam * event_params_p = reflected_event.get_param_array();
  UFunction * ue_function_p = reflected_event.m_ue_function_p.Get(); // Invoke the first one
  #if WITH_EDITORONLY_DATA
    if (!ue_function_p)
      {
      ue_function_p = find_ue_function(reflected_event.m_sk_invokable_p);
      SK_ASSERTX(ue_function_p, a_str_format("Cannot find UE counterpart of method %s@%s!", reflected_event.m_sk_invokable_p->get_scope()->get_name_cstr(), reflected_event.m_sk_invokable_p->get_name_cstr()));
      }
  #endif
  uint8_t * k2_params_storage_p = a_stack_allocate(ue_function_p->ParmsSize, uint8_t);
  for (uint32_t i = 0; i < reflected_event.m_num_params; ++i)
    {
    const ReflectedEventParam & event_param = event_params_p[i];
    (*event_param.m_storer_p)(k2_params_storage_p + event_param.m_offset, scope_p->get_arg(i), event_param);
    }

  // Invoke K2 script event with parameters
  AActor * actor_p = scope_p->this_as<SkUEActor>();
  UFunction * ue_function_to_invoke_p = reflected_event.m_ue_function_to_invoke_p.Get();
  #if WITH_EDITORONLY_DATA
    if (!ue_function_to_invoke_p)
      {
      // Find Kismet copy of our method to invoke
      ue_function_to_invoke_p = actor_p->FindFunctionChecked(*ue_function_p->GetName());
      reflected_event.m_ue_function_to_invoke_p = ue_function_to_invoke_p;
      }
  #endif
  // Check if this event is actually present in any Blueprint graph
  SK_ASSERTX(ue_function_to_invoke_p->Script.Num() > 0, a_str_format("Warning: Call to '%S' on actor '%S' has no effect as no Blueprint event node named '%S' exists in any of its event graphs.", *ue_function_p->GetName(), *actor_p->GetName(), *ue_function_p->GetName()));
  // Call the event function
  actor_p->ProcessEvent(ue_function_to_invoke_p, k2_params_storage_p);

  // Copy back any outgoing parameters
  if (ue_function_to_invoke_p->HasAllFunctionFlags(FUNC_HasOutParms))
    {
    for (uint32_t i = 0; i < reflected_event.m_num_params; ++i)
      {
      const ReflectedEventParam & event_param = event_params_p[i];
      if (event_param.m_assigner_p)
        {
        (*event_param.m_assigner_p)(scope_p->get_arg(i), k2_params_storage_p + event_param.m_offset, event_param);
        }
      }
    }

  // And pass back the result
  if (result_pp)
    {
    if (reflected_event.m_result_getter)
      {
      *result_pp = (*reflected_event.m_result_getter)((uint8 *)k2_params_storage_p + ue_function_to_invoke_p->ReturnValueOffset, reflected_event.m_result_type);
      }
    else
      {
      *result_pp = SkBrain::ms_nil_p;
      }
    }
  return;
  }

//---------------------------------------------------------------------------------------

template<class _TypedName>
bool SkUEReflectionManager::have_identical_signatures(const tSkParamList & param_list, const _TypedName * param_array_p)
  {
  for (uint32_t i = 0; i < param_list.get_length(); ++i)
    {
    const TypedName & typed_name = param_array_p[i];
    const SkParameterBase * param_p = param_list[i];
    if (typed_name.get_name() != param_p->get_name()
     || typed_name.m_sk_class_name != param_p->get_expected_type()->get_key_class()->get_name())
      {
      return false;
      }
    }

  return true;
  }

//---------------------------------------------------------------------------------------
// Re-resolve class pointers for params
template<class _TypedName>
void SkUEReflectionManager::rebind_params_to_sk(const tSkParamList & param_list, _TypedName * param_array_p)
  {
  for (uint32_t i = 0; i < param_list.get_length(); ++i)
    {
    const SkParameterBase * param_p = param_list[i];
    TypedName & typed_name = param_array_p[i];
    SK_ASSERTX(typed_name.get_name() == param_p->get_name() && typed_name.m_sk_class_name == param_p->get_expected_type()->get_key_class()->get_name(), "Caller must ensure beforehand that signatures match.");
    typed_name.m_sk_class_p = param_p->get_expected_type()->get_key_class();
    }
  }

//---------------------------------------------------------------------------------------
// Store a given ReflectedFunction into the m_binding_entry_array
// If an index is given, use that, otherwise, find an empty slot to reuse, or if that fails, append a new entry
int32_t SkUEReflectionManager::store_reflected_function(ReflectedFunction * reflected_function_p, ReflectedClass * reflected_class_p, int32_t function_index_to_use)
  {
  // If no binding index known yet, look if there is an empty slot that we can reuse
  if (function_index_to_use < 0)
    {
    for (function_index_to_use = 0; function_index_to_use < (int32_t)m_reflected_functions.get_length(); ++function_index_to_use)
      {
      if (!m_reflected_functions[function_index_to_use]) break;
      }
    }
  if (function_index_to_use == m_reflected_functions.get_length())
    {
    m_reflected_functions.append(*reflected_function_p);
    }
  else
    {
    m_reflected_functions.set_at(function_index_to_use, reflected_function_p);
    }

  // Remember binding index to invoke Blueprint events
  reflected_function_p->m_sk_invokable_p->set_user_data(function_index_to_use);

  // Hook into class
  if (!reflected_class_p)
    {
    SK_ASSERTX(!m_reflected_classes.get(reflected_function_p->m_sk_invokable_p->get_scope()->get_name()), "Reflected class exists even though reflected_class_p was passed in as nullptr.");
    reflected_class_p = new ReflectedClass(reflected_function_p->m_sk_invokable_p->get_scope()->get_name());
    m_reflected_classes.append(*reflected_class_p);
    }
  reflected_class_p->m_functions.append(FunctionIndex(function_index_to_use));

  return function_index_to_use;
  }

//---------------------------------------------------------------------------------------
// Delete reflected function and set pointer to nullptr so it can be reused
// Note: Does _not_ remove this function's entry from its reflected class!
void SkUEReflectionManager::delete_reflected_function(uint32_t function_index)
  {
  ReflectedFunction * reflected_function_p = m_reflected_functions[function_index];
  if (reflected_function_p)
    {
    //SK_ASSERTX(reflected_function_p->m_ue_function_p.IsValid(), a_str_format("UFunction %s was deleted outside of SkUEReflectionManager and left dangling links behind in its owner UClass (%s).", reflected_function_p->get_name_cstr(), reflected_function_p->m_sk_invokable_p->get_scope()->get_name_cstr()));
    if (reflected_function_p->m_ue_function_p.IsValid())
      {
      UFunction * ue_function_p = reflected_function_p->m_ue_function_p.Get();
      UClass * ue_class_p = ue_function_p->GetOwnerClass();
      // Unlink from its owner class
      ue_class_p->RemoveFunctionFromFunctionMap(ue_function_p);
      // Unlink from the Children list as well
      UField ** prev_field_pp = &ue_class_p->Children;
      for (UField * field_p = *prev_field_pp; field_p; prev_field_pp = &field_p->Next, field_p = *prev_field_pp)
        {
        if (field_p == ue_function_p)
          {
          *prev_field_pp = field_p->Next;
          break;
          }
        }

      // Destroy the function along with its attached properties
      // HACK remove from root if it's rooted - proper fix: Find out why it's rooted to begin with
      ue_function_p->RemoveFromRoot();
      ue_function_p->MarkPendingKill();
      }

    FMemory::Free(reflected_function_p);
    m_reflected_functions[function_index] = nullptr;
    }
  }

//---------------------------------------------------------------------------------------

UFunction * SkUEReflectionManager::find_ue_function(SkInvokableBase * sk_invokable_p)
  {
  UClass * ue_class_p = SkUEClassBindingHelper::get_ue_class_from_sk_class(sk_invokable_p->get_scope());
  if (!ue_class_p) return nullptr;

  FString ue_function_name;
  AString sk_function_name = sk_invokable_p->get_name_str();
  for (TFieldIterator<UFunction> func_it(ue_class_p, EFieldIteratorFlags::ExcludeSuper); func_it; ++func_it)
    {
    UFunction * ue_function_p = *func_it;
    ue_function_p->GetName(ue_function_name);
    if (FSkookumScriptGeneratorHelper::compare_var_name_skookified(*ue_function_name, sk_function_name.as_cstr()))
      {
      return ue_function_p;
      }
    }

  return nullptr;
  }

//---------------------------------------------------------------------------------------

UFunction * SkUEReflectionManager::reflect_ue_function(SkInvokableBase * sk_invokable_p, ReflectedProperty * out_param_info_array_p, bool is_final)
  {
  // Find the function
  UFunction * ue_function_p = find_ue_function(sk_invokable_p);
  if (!ue_function_p) return nullptr;

  // Now, build reflected parameter list
  const tSkParamList & param_list = sk_invokable_p->get_params().get_param_list();
  uint32_t num_parameters = 0;
  for (TFieldIterator<UProperty> param_it(ue_function_p); param_it; ++param_it)
    {
    UProperty * param_p = *param_it;
    if ((param_p->GetPropertyFlags() & (CPF_ReturnParm|CPF_Parm)) == CPF_Parm)
      {
      // Too many parameters?
      if (num_parameters > param_list.get_length())
        {
        return nullptr;
        }

      // Reflect this parameter and check if successful
      ReflectedProperty * out_param_info_p = out_param_info_array_p + num_parameters;
      if (!reflect_ue_property(param_p, out_param_info_p)
       || out_param_info_p->get_name() != param_list[num_parameters]->get_name())
        {
        return nullptr;
        }

      // Got one more parameter
      ++num_parameters;
      }
    }

  // Did we find fewer parameters than we need?
  if (num_parameters < param_list.get_length())
    {
    return nullptr;
    }

  return ue_function_p;
  }

//---------------------------------------------------------------------------------------

bool SkUEReflectionManager::reflect_ue_property(UProperty * ue_property_p, ReflectedProperty * out_info_p)
  {
  // Based on Sk type, figure out the matching UProperty as well as fetcher and setter methods
  tK2ParamFetcher  k2_param_fetcher_p = nullptr;
  tK2ValueFetcher  k2_value_fetcher_p = nullptr;
  tK2ValueAssigner k2_value_assigner_p = nullptr;
  tSkValueStorer   sk_value_storer_p = nullptr;

  if (ue_property_p->IsA<UBoolProperty>())
    {
    k2_param_fetcher_p  = &fetch_k2_param_boolean;
    k2_value_fetcher_p  = &fetch_k2_value_boolean;
    k2_value_assigner_p = &assign_k2_value_boolean;
    sk_value_storer_p   = &store_sk_value_boolean;
    }
  else if (ue_property_p->IsA<UIntProperty>())
    {
    k2_param_fetcher_p  = &fetch_k2_param_integer;
    k2_value_fetcher_p  = &fetch_k2_value_integer;
    k2_value_assigner_p = &assign_k2_value_integer;
    sk_value_storer_p   = &store_sk_value_integer;
    }
  else if (ue_property_p->IsA<UFloatProperty>())
    {
    k2_param_fetcher_p  = &fetch_k2_param_real;
    k2_value_fetcher_p  = &fetch_k2_value_real;
    k2_value_assigner_p = &assign_k2_value_real;
    sk_value_storer_p   = &store_sk_value_real;
    }
  else if (ue_property_p->IsA<UStrProperty>())
    {
    k2_param_fetcher_p  = &fetch_k2_param_string;
    k2_value_fetcher_p  = &fetch_k2_value_string;
    k2_value_assigner_p = &assign_k2_value_string;
    sk_value_storer_p   = &store_sk_value_string;
    }
  else if (ue_property_p->IsA<UStructProperty>())
    {
    UScriptStruct * struct_p = static_cast<UStructProperty *>(ue_property_p)->Struct;
    if (struct_p->GetFName() == ms_struct_vector2_p->GetFName())
      {
      k2_param_fetcher_p  = &fetch_k2_param_vector2;
      k2_value_fetcher_p  = &fetch_k2_value_vector2;
      k2_value_assigner_p = &assign_k2_value_vector2;
      sk_value_storer_p   = &store_sk_value_vector2;
      }
    else if (struct_p->GetFName() == ms_struct_vector3_p->GetFName())
      {
      k2_param_fetcher_p  = &fetch_k2_param_vector3;
      k2_value_fetcher_p  = &fetch_k2_value_vector3;
      k2_value_assigner_p = &assign_k2_value_vector3;
      sk_value_storer_p   = &store_sk_value_vector3;
      }
    else if (struct_p->GetFName() == ms_struct_vector4_p->GetFName())
      {
      k2_param_fetcher_p  = &fetch_k2_param_vector4;
      k2_value_fetcher_p  = &fetch_k2_value_vector4;
      k2_value_assigner_p = &assign_k2_value_vector4;
      sk_value_storer_p   = &store_sk_value_vector4;
      }
    else if (struct_p->GetFName() == ms_struct_rotation_angles_p->GetFName())
      {
      k2_param_fetcher_p  = &fetch_k2_param_rotation_angles;
      k2_value_fetcher_p  = &fetch_k2_value_rotation_angles;
      k2_value_assigner_p = &assign_k2_value_rotation_angles;
      sk_value_storer_p   = &store_sk_value_rotation_angles;
      }
    else if (struct_p->GetFName() == ms_struct_transform_p->GetFName())
      {
      k2_param_fetcher_p  = &fetch_k2_param_transform;
      k2_value_fetcher_p  = &fetch_k2_value_transform;
      k2_value_assigner_p = &assign_k2_value_transform;
      sk_value_storer_p   = &store_sk_value_transform;
      }
    else if (!struct_p->IsA<UUserDefinedStruct>()) // MJB reject UUserDefinedStructs for now
      {
      if (SkInstance::is_data_stored_by_val(struct_p->GetStructureSize()))
        {
        k2_param_fetcher_p  = &fetch_k2_param_struct_val;
        k2_value_fetcher_p  = &fetch_k2_value_struct_val;
        k2_value_assigner_p = &assign_k2_value_struct_val;
        sk_value_storer_p   = &store_sk_value_struct_val;
        }
      else
        {
        k2_param_fetcher_p  = &fetch_k2_param_struct_ref;
        k2_value_fetcher_p  = &fetch_k2_value_struct_ref;
        k2_value_assigner_p = &assign_k2_value_struct_ref;
        sk_value_storer_p   = &store_sk_value_struct_ref;
        }
      }
    }
  else if (ue_property_p->IsA<UByteProperty>() && static_cast<UByteProperty *>(ue_property_p)->Enum)
    {
    k2_param_fetcher_p  = &fetch_k2_param_enum;
    k2_value_fetcher_p  = &fetch_k2_value_enum;
    k2_value_assigner_p = &assign_k2_value_enum;
    sk_value_storer_p   = &store_sk_value_enum;
    }
  else if (ue_property_p->IsA<UObjectProperty>())
    {
    k2_param_fetcher_p  = &fetch_k2_param_entity;
    k2_value_fetcher_p  = &fetch_k2_value_entity;
    k2_value_assigner_p = &assign_k2_value_entity;
    sk_value_storer_p   = &store_sk_value_entity;
    }

  // Set result
  if (k2_param_fetcher_p && out_info_p)
    {
    FString var_name = FSkookumScriptGeneratorHelper::skookify_var_name(ue_property_p->GetName(), ue_property_p->IsA(UBoolProperty::StaticClass()), FSkookumScriptGeneratorHelper::VarScope_local);
    out_info_p->set_name(ASymbol::create(FStringToAString(var_name)));
    out_info_p->m_ue_property_p       = ue_property_p;
    out_info_p->m_k2_param_fetcher_p  = k2_param_fetcher_p;
    out_info_p->m_k2_value_fetcher_p  = k2_value_fetcher_p;
    out_info_p->m_k2_value_assigner_p = k2_value_assigner_p;
    out_info_p->m_sk_value_storer_p   = sk_value_storer_p;
    }

  return k2_param_fetcher_p != nullptr;
  }

//---------------------------------------------------------------------------------------
// Params:
//   out_param_info_array_p: Storage for info on each parameter, return value is stored behind the input parameters, and is zeroed if there is no return value
UFunction * SkUEReflectionManager::build_ue_function(UClass * ue_class_p, SkInvokableBase * sk_invokable_p, eReflectedFunctionType binding_type, uint32_t binding_index, ReflectedProperty * out_param_info_array_p, bool is_final)
  {
  // Build name of method including scope
  const char * invokable_name_p = sk_invokable_p->get_name_cstr();
  const char * class_name_p = sk_invokable_p->get_scope()->get_name_cstr();
  AString qualified_invokable_name;
  qualified_invokable_name.ensure_size_buffer(uint32_t(::strlen(invokable_name_p) + ::strlen(class_name_p) + 3u));
  qualified_invokable_name.append(class_name_p);
  qualified_invokable_name.append(" @ ", 3u);
  qualified_invokable_name.append(invokable_name_p);
  FName qualified_invokable_fname(qualified_invokable_name.as_cstr());

  // Must not be already present
  #if WITH_EDITORONLY_DATA
    UFunction * old_ue_function_p = ue_class_p->FindFunctionByName(qualified_invokable_fname);
    if (old_ue_function_p)
      {
      ue_class_p->ClearFunctionMapsCaches();
      old_ue_function_p = ue_class_p->FindFunctionByName(qualified_invokable_fname);
      SK_MAD_ASSERTX(!old_ue_function_p, a_str_format("Found reflected duplicate of function %S@%s!", *ue_class_p->GetName(), qualified_invokable_name.as_cstr()));
      }
  #endif

  // Make UFunction object
  UFunction * ue_function_p = NewObject<UFunction>(ue_class_p, qualified_invokable_fname, RF_Public);

  ue_function_p->FunctionFlags |= FUNC_Public;
  if (sk_invokable_p->is_class_member())
    {
    ue_function_p->FunctionFlags |= FUNC_Static;
    }

  if (binding_type == ReflectedFunctionType_Call)
    {
    ue_function_p->FunctionFlags |= FUNC_BlueprintCallable | FUNC_Native;
    ue_function_p->SetNativeFunc(sk_invokable_p->get_invoke_type() == SkInvokable_coroutine 
      ? (Native)&SkUEReflectionManager::exec_coroutine
      : (sk_invokable_p->is_class_member() ? (Native)&SkUEReflectionManager::exec_class_method : (Native)&SkUEReflectionManager::exec_instance_method));
    #if WITH_EDITOR
      ue_function_p->SetMetaData(TEXT("Tooltip"), *FString::Printf(TEXT("%S\n%S@%S()"), 
        sk_invokable_p->get_invoke_type() == SkInvokable_coroutine ? "Kick off SkookumScript coroutine" : "Call to SkookumScript method", 
        sk_invokable_p->get_scope()->get_name_cstr(), 
        sk_invokable_p->get_name_cstr()));
    #endif
    ue_function_p->RepOffset = (uint16_t)binding_index; // Remember binding index here for later lookup
    }
  else // binding_type == BindingType_Event
    {
    ue_function_p->FunctionFlags |= FUNC_BlueprintEvent | FUNC_Event;
    ue_function_p->Bind(); // Bind to default Blueprint event mechanism
    #if WITH_EDITOR
      ue_function_p->SetMetaData(TEXT("Tooltip"), *FString::Printf(TEXT("Triggered by SkookumScript method\n%S@%S()"), sk_invokable_p->get_scope()->get_name_cstr(), sk_invokable_p->get_name_cstr()));
    #endif    
    ue_function_p->RepOffset = EventMagicRepOffset; // So we can tell later this is an Sk event
    }

  #if WITH_EDITOR
    ue_function_p->SetMetaData(TEXT("Category"), TEXT("SkookumScript"));
  #endif

  // Parameters of the method we are creating
  const SkParameters & params = sk_invokable_p->get_params();
  const tSkParamList & param_list = params.get_param_list();
  uint32_t num_params = param_list.get_length();

  // Handle return value if any
  if (params.get_result_class() && params.get_result_class() != SkBrain::ms_object_class_p)
    {
    UProperty * result_param_p = build_ue_param(m_result_name, params.get_result_class(), ue_function_p, out_param_info_array_p ? out_param_info_array_p + num_params : nullptr, is_final);
    if (!result_param_p)
      {
      // If any parameters can not be mapped, skip building this entire function
      ue_function_p->MarkPendingKill();
      return nullptr;
      }

    result_param_p->PropertyFlags |= CPF_ReturnParm; // Flag as return value
    }

  // Handle input parameters (in reverse order so they get linked into the list in proper order)
  for (int32_t i = num_params - 1; i >= 0; --i)
    {
    const SkParameterBase * input_param_p = param_list[i];
    if (!build_ue_param(input_param_p->get_name(), input_param_p->get_expected_type(), ue_function_p, out_param_info_array_p ? out_param_info_array_p + i : nullptr, is_final))
      {
      // If any parameters can not be mapped, skip building this entire function
      ue_function_p->MarkPendingKill();
      return nullptr;
      }
    }

  // Make method known to its class
  ue_class_p->LinkChild(ue_function_p);
  ue_class_p->AddFunctionToFunctionMap(ue_function_p);

  // Make sure parameter list is properly linked and offsets are set
  ue_function_p->StaticLink(true);

  return ue_function_p;
  }

//---------------------------------------------------------------------------------------

UProperty * SkUEReflectionManager::build_ue_param(const ASymbol & sk_name, SkClassDescBase * sk_type_p, UFunction * ue_function_p, ReflectedProperty * out_info_p, bool is_final)
  {
  // Build property
  UProperty * property_p = build_ue_property(sk_name, sk_type_p, ue_function_p, out_info_p, is_final);

  // Add flags and attach to function
  if (property_p)
    {
    property_p->PropertyFlags |= CPF_Parm;
    ue_function_p->LinkChild(property_p);
    }

  return property_p;
  }

//---------------------------------------------------------------------------------------

UProperty * SkUEReflectionManager::build_ue_property(const ASymbol & sk_name, SkClassDescBase * sk_type_p, UObject * ue_outer_p, ReflectedProperty * out_info_p, bool is_final)
  {
  // Based on Sk type, figure out the matching UProperty as well as fetcher and setter methods
  UProperty *      ue_property_p = nullptr;
  tK2ParamFetcher  k2_param_fetcher_p = nullptr;
  tK2ValueFetcher  k2_value_fetcher_p = nullptr;
  tK2ValueAssigner k2_value_assigner_p = nullptr;
  tSkValueStorer   sk_value_storer_p = nullptr;
  
  FName ue_name(sk_name.as_cstr());

  if (sk_type_p == SkBoolean::get_class())
    {
    ue_property_p = NewObject<UBoolProperty>(ue_outer_p, ue_name, RF_Public);
    k2_param_fetcher_p  = &fetch_k2_param_boolean;
    k2_value_fetcher_p  = &fetch_k2_value_boolean;
    k2_value_assigner_p = &assign_k2_value_boolean;
    sk_value_storer_p   = &store_sk_value_boolean;
    }
  else if (sk_type_p == SkInteger::get_class())
    {
    ue_property_p = NewObject<UIntProperty>(ue_outer_p, ue_name, RF_Public);
    k2_param_fetcher_p  = &fetch_k2_param_integer;
    k2_value_fetcher_p  = &fetch_k2_value_integer;
    k2_value_assigner_p = &assign_k2_value_integer;
    sk_value_storer_p   = &store_sk_value_integer;
    }
  else if (sk_type_p == SkReal::get_class())
    {
    ue_property_p = NewObject<UFloatProperty>(ue_outer_p, ue_name, RF_Public);
    k2_param_fetcher_p  = &fetch_k2_param_real;
    k2_value_fetcher_p  = &fetch_k2_value_real;
    k2_value_assigner_p = &assign_k2_value_real;
    sk_value_storer_p   = &store_sk_value_real;
    }
  else if (sk_type_p == SkString::get_class())
    {
    ue_property_p = NewObject<UStrProperty>(ue_outer_p, ue_name, RF_Public);
    k2_param_fetcher_p  = &fetch_k2_param_string;
    k2_value_fetcher_p  = &fetch_k2_value_string;
    k2_value_assigner_p = &assign_k2_value_string;
    sk_value_storer_p   = &store_sk_value_string;
    }
  else if (sk_type_p == SkVector2::get_class())
    {
    ue_property_p = NewObject<UStructProperty>(ue_outer_p, ue_name);
    static_cast<UStructProperty *>(ue_property_p)->Struct = ms_struct_vector2_p;
    k2_param_fetcher_p  = &fetch_k2_param_vector2;
    k2_value_fetcher_p  = &fetch_k2_value_vector2;
    k2_value_assigner_p = &assign_k2_value_vector2;
    sk_value_storer_p   = &store_sk_value_vector2;
    }
  else if (sk_type_p == SkVector3::get_class())
    {
    ue_property_p = NewObject<UStructProperty>(ue_outer_p, ue_name);
    static_cast<UStructProperty *>(ue_property_p)->Struct = ms_struct_vector3_p;
    k2_param_fetcher_p  = &fetch_k2_param_vector3;
    k2_value_fetcher_p  = &fetch_k2_value_vector3;
    k2_value_assigner_p = &assign_k2_value_vector3;
    sk_value_storer_p   = &store_sk_value_vector3;
    }
  else if (sk_type_p == SkVector4::get_class())
    {
    ue_property_p = NewObject<UStructProperty>(ue_outer_p, ue_name);
    static_cast<UStructProperty *>(ue_property_p)->Struct = ms_struct_vector4_p;
    k2_param_fetcher_p  = &fetch_k2_param_vector4;
    k2_value_fetcher_p  = &fetch_k2_value_vector4;
    k2_value_assigner_p = &assign_k2_value_vector4;
    sk_value_storer_p   = &store_sk_value_vector4;
    }
  else if (sk_type_p == SkRotationAngles::get_class())
    {
    ue_property_p = NewObject<UStructProperty>(ue_outer_p, ue_name);
    static_cast<UStructProperty *>(ue_property_p)->Struct = ms_struct_rotation_angles_p;
    k2_param_fetcher_p  = &fetch_k2_param_rotation_angles;
    k2_value_fetcher_p  = &fetch_k2_value_rotation_angles;
    k2_value_assigner_p = &assign_k2_value_rotation_angles;
    sk_value_storer_p   = &store_sk_value_rotation_angles;
    }
  else if (sk_type_p == SkTransform::get_class())
    {
    ue_property_p = NewObject<UStructProperty>(ue_outer_p, ue_name);
    static_cast<UStructProperty *>(ue_property_p)->Struct = ms_struct_transform_p;
    k2_param_fetcher_p  = &fetch_k2_param_transform;
    k2_value_fetcher_p  = &fetch_k2_value_transform;
    k2_value_assigner_p = &assign_k2_value_transform;
    sk_value_storer_p   = &store_sk_value_transform;
    }
  else if (sk_type_p->get_key_class()->is_class(*SkEnum::get_class()))
    {
    UEnum * ue_enum_p = FindObject<UEnum>(ANY_PACKAGE, *FString(sk_type_p->get_key_class_name().as_cstr()));
    if (ue_enum_p)
      {
      ue_property_p = NewObject<UByteProperty>(ue_outer_p, ue_name);
      static_cast<UByteProperty *>(ue_property_p)->Enum = ue_enum_p;
      k2_param_fetcher_p  = &fetch_k2_param_enum;
      k2_value_fetcher_p  = &fetch_k2_value_enum;
      k2_value_assigner_p = &assign_k2_value_enum;
      sk_value_storer_p   = &store_sk_value_enum;
      }
    else if (is_final)
      {
      on_unknown_type(sk_name, sk_type_p, ue_outer_p);
      }
    }
  else if (sk_type_p->get_key_class()->is_class(*SkUEEntity::get_class()))
    {
    UClass * ue_class_p = SkUEClassBindingHelper::get_ue_class_from_sk_class(sk_type_p->get_key_class());
    if (ue_class_p)
      {
      ue_property_p = NewObject<UObjectProperty>(ue_outer_p, ue_name, RF_Public);
      static_cast<UObjectProperty *>(ue_property_p)->PropertyClass = ue_class_p;
      k2_param_fetcher_p  = &fetch_k2_param_entity;
      k2_value_fetcher_p  = &fetch_k2_value_entity;
      k2_value_assigner_p = &assign_k2_value_entity;
      sk_value_storer_p   = &store_sk_value_entity;
      }
    else if (is_final)
      {
      on_unknown_type(sk_name, sk_type_p, ue_outer_p);
      }
    }
  else
    {
    UStruct * ue_struct_p = SkUEClassBindingHelper::get_ue_struct_from_sk_class(sk_type_p->get_key_class());
    if (ue_struct_p)
      {
      ue_property_p = NewObject<UStructProperty>(ue_outer_p, ue_name);
      static_cast<UStructProperty *>(ue_property_p)->Struct = CastChecked<UScriptStruct>(ue_struct_p);
      if (SkInstance::is_data_stored_by_val(ue_struct_p->GetStructureSize()))
        {
        k2_param_fetcher_p  = &fetch_k2_param_struct_val;
        k2_value_fetcher_p  = &fetch_k2_value_struct_val;
        k2_value_assigner_p = &assign_k2_value_struct_val;
        sk_value_storer_p   = &store_sk_value_struct_val;
        }
      else
        {
        k2_param_fetcher_p  = &fetch_k2_param_struct_ref;
        k2_value_fetcher_p  = &fetch_k2_value_struct_ref;
        k2_value_assigner_p = &assign_k2_value_struct_ref;
        sk_value_storer_p   = &store_sk_value_struct_ref;
        }
      }
    else if (is_final)
      {
      on_unknown_type(sk_name, sk_type_p, ue_outer_p);
      }
    }

  // Set result
  if (out_info_p)
    {
    out_info_p->set_name(sk_name);
    out_info_p->m_ue_property_p       = ue_property_p;
    out_info_p->m_k2_param_fetcher_p  = k2_param_fetcher_p;
    out_info_p->m_k2_value_fetcher_p  = k2_value_fetcher_p;
    out_info_p->m_k2_value_assigner_p = k2_value_assigner_p;
    out_info_p->m_sk_value_storer_p   = sk_value_storer_p;
    }

  return ue_property_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::bind_event_method(SkMethodBase * sk_method_p)
  {
  SK_ASSERTX(!sk_method_p->is_bound() || static_cast<SkMethodFunc *>(sk_method_p)->m_atomic_f == &mthd_trigger_event, a_str_format("Trying to bind Blueprint event method '%s' but it is already bound to a different atomic implementation!", sk_method_p->get_name_cstr_dbg()));
  if (!sk_method_p->is_bound())
    {
    sk_method_p->get_scope()->register_method_func(sk_method_p->get_name(), &mthd_trigger_event, sk_method_p->is_class_member() ? SkBindFlag_class_no_rebind : SkBindFlag_instance_no_rebind);
    }
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::on_unknown_type(const ASymbol & sk_name, SkClassDescBase * sk_type_p, UObject * ue_outer_p)
  {
  #if (SKOOKUM & SK_DEBUG)
    UFunction * ue_function_p = Cast<UFunction>(ue_outer_p);
    if (ue_function_p)
      {
      SK_ERRORX(a_cstr_format("Type '%s' of parameter '%s' of method '%S.%S' being exported to Blueprints can not be mapped to a Blueprint-compatible type.", sk_type_p->get_key_class_name().as_cstr_dbg(), sk_name.as_cstr(), *ue_function_p->GetOwnerClass()->GetName(), *ue_function_p->GetName()));
      }
    UClass * ue_class_p = Cast<UClass>(ue_outer_p);
    if (ue_class_p)
      {
      SK_ERRORX(a_cstr_format("Type '%s' of data member '%s' of class '%S' being exported to Blueprints can not be mapped to a Blueprint-compatible type.", sk_type_p->get_key_class_name().as_cstr_dbg(), sk_name.as_cstr(), *ue_class_p->GetName()));
      }
  #endif
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_boolean(FFrame & stack, const TypedName & typed_name)
  {
  UBoolProperty::TCppType value = UBoolProperty::GetDefaultPropertyValue();
  stack.StepCompiledIn<UBoolProperty>(&value);
  return SkBoolean::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_integer(FFrame & stack, const TypedName & typed_name)
  {
  UIntProperty::TCppType value = UIntProperty::GetDefaultPropertyValue();
  stack.StepCompiledIn<UIntProperty>(&value);
  return SkInteger::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_real(FFrame & stack, const TypedName & typed_name)
  {
  UFloatProperty::TCppType value = UFloatProperty::GetDefaultPropertyValue();
  stack.StepCompiledIn<UFloatProperty>(&value);
  return SkReal::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_string(FFrame & stack, const TypedName & typed_name)
  {
  UStrProperty::TCppType value = UStrProperty::GetDefaultPropertyValue();
  stack.StepCompiledIn<UStrProperty>(&value);
  return SkString::new_instance(FStringToAString(value));
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_vector2(FFrame & stack, const TypedName & typed_name)
  {
  FVector2D value(ForceInitToZero);
  stack.StepCompiledIn<UStructProperty>(&value);
  return SkVector2::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_vector3(FFrame & stack, const TypedName & typed_name)
  {
  FVector value(ForceInitToZero);
  stack.StepCompiledIn<UStructProperty>(&value);
  return SkVector3::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_vector4(FFrame & stack, const TypedName & typed_name)
  {
  FVector4 value(ForceInitToZero);
  stack.StepCompiledIn<UStructProperty>(&value);
  return SkVector4::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_rotation_angles(FFrame & stack, const TypedName & typed_name)
  {
  FRotator value(ForceInitToZero);
  stack.StepCompiledIn<UStructProperty>(&value);
  return SkRotationAngles::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_transform(FFrame & stack, const TypedName & typed_name)
  {
  FTransform value;
  stack.StepCompiledIn<UStructProperty>(&value);
  return SkTransform::new_instance(value);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_struct_val(FFrame & stack, const TypedName & typed_name)
  {
  void * user_data_p;
  SkInstance * instance_p = SkInstance::new_instance_uninitialized_val(typed_name.m_sk_class_p, typed_name.m_byte_size, &user_data_p);
  stack.StepCompiledIn<UStructProperty>(user_data_p);
  return instance_p;
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_struct_ref(FFrame & stack, const TypedName & typed_name)
  {
  void * user_data_p;
  SkInstance * instance_p = SkInstance::new_instance_uninitialized_ref(typed_name.m_sk_class_p, typed_name.m_byte_size, &user_data_p);
  stack.StepCompiledIn<UStructProperty>(user_data_p);
  return instance_p;
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_entity(FFrame & stack, const TypedName & typed_name)
  {
  UObject * obj_p = nullptr;
  stack.StepCompiledIn<UObjectPropertyBase>(&obj_p);
  return SkUEEntity::new_instance(obj_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_param_enum(FFrame & stack, const TypedName & typed_name)
  {
  UByteProperty::TCppType value = UByteProperty::GetDefaultPropertyValue();
  stack.StepCompiledIn<UByteProperty>(&value);
  SkInstance * instance_p = typed_name.m_sk_class_p->new_instance();
  instance_p->construct<SkEnum>(SkEnumType(value));  
  return instance_p;
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_boolean(const void * value_p, const TypedName & typed_name)
  {
  return SkBoolean::new_instance(*(const UBoolProperty::TCppType *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_integer(const void * value_p, const TypedName & typed_name)
  {
  return SkInteger::new_instance(*(const UIntProperty::TCppType *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_real(const void * value_p, const TypedName & typed_name)
  {
  return SkReal::new_instance(*(const UFloatProperty::TCppType *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_string(const void * value_p, const TypedName & typed_name)
  {
  return SkString::new_instance(FStringToAString(*(const UStrProperty::TCppType *)value_p));
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_vector2(const void * value_p, const TypedName & typed_name)
  {
  return SkVector2::new_instance(*(const FVector2D *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_vector3(const void * value_p, const TypedName & typed_name)
  {
  return SkVector3::new_instance(*(const FVector *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_vector4(const void * value_p, const TypedName & typed_name)
  {
  return SkVector4::new_instance(*(const FVector4 *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_rotation_angles(const void * value_p, const TypedName & typed_name)
  {
  return SkRotationAngles::new_instance(*(const FRotator *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_transform(const void * value_p, const TypedName & typed_name)
  {
  return SkTransform::new_instance(*(const FTransform *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_struct_val(const void * value_p, const TypedName & typed_name)
  {
  void * user_data_p;
  SkInstance * instance_p = SkInstance::new_instance_uninitialized_val(typed_name.m_sk_class_p, typed_name.m_byte_size, &user_data_p);
  FMemory::Memcpy(reinterpret_cast<uint32_t *>(user_data_p), reinterpret_cast<const uint32_t *>(value_p), typed_name.m_byte_size);
  return instance_p;
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_struct_ref(const void * value_p, const TypedName & typed_name)
  {
  void * user_data_p;
  SkInstance * instance_p = SkInstance::new_instance_uninitialized_ref(typed_name.m_sk_class_p, typed_name.m_byte_size, &user_data_p);
  FMemory::Memcpy(reinterpret_cast<uint32_t *>(user_data_p), reinterpret_cast<const uint32_t *>(value_p), typed_name.m_byte_size);
  return instance_p;
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_entity(const void * value_p, const TypedName & typed_name)
  {
  return SkUEEntity::new_instance(*(UObject * const *)value_p);
  }

//---------------------------------------------------------------------------------------

SkInstance * SkUEReflectionManager::fetch_k2_value_enum(const void * value_p, const TypedName & typed_name)
  {
  SkInstance * instance_p = typed_name.m_sk_class_p->new_instance();
  instance_p->construct<SkEnum>(SkEnumType(*(const UByteProperty::TCppType *)value_p));
  return instance_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_boolean(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkBoolean>() = *(const UBoolProperty::TCppType *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_integer(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkInteger>() = *(const UIntProperty::TCppType *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_real(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkReal>() = *(const UFloatProperty::TCppType *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_string(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkString>() = FStringToAString(*(const UStrProperty::TCppType *)value_p);
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_vector2(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkVector2>() = *(const FVector2D *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_vector3(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkVector3>() = *(const FVector *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_vector4(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkVector4>() = *(const FVector4 *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_rotation_angles(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkRotationAngles>() = *(const FRotator *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_transform(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkTransform>() = *(const FTransform *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_struct_val(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  FMemory::Memcpy(reinterpret_cast<uint32_t *>(dest_p->get_raw_pointer_val()), reinterpret_cast<const uint32_t *>(value_p), typed_name.m_byte_size);
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_struct_ref(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  FMemory::Memcpy(reinterpret_cast<uint32_t *>(dest_p->get_raw_pointer_ref()), reinterpret_cast<const uint32_t *>(value_p), typed_name.m_byte_size);
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_entity(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkUEEntity>() = *(UObject * const *)value_p;
  }

//---------------------------------------------------------------------------------------

void SkUEReflectionManager::assign_k2_value_enum(SkInstance * dest_p, const void * value_p, const TypedName & typed_name)
  {
  dest_p->as<SkEnum>() = SkEnumType(*(const UByteProperty::TCppType *)value_p);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_boolean(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  *((UBoolProperty::TCppType *)dest_p) = value_p->as<SkBoolean>();
  return sizeof(UBoolProperty::TCppType);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_integer(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  *((UIntProperty::TCppType *)dest_p) = value_p->as<SkInteger>();
  return sizeof(UIntProperty::TCppType);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_real(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  *((UFloatProperty::TCppType *)dest_p) = value_p->as<SkReal>();
  return sizeof(UFloatProperty::TCppType);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_string(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  new (dest_p) UStrProperty::TCppType(value_p->as<SkString>().as_cstr());
  return sizeof(UStrProperty::TCppType);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_vector2(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  new (dest_p) FVector2D(value_p->as<SkVector2>());
  return sizeof(FVector2D);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_vector3(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  new (dest_p) FVector(value_p->as<SkVector3>());
  return sizeof(FVector);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_vector4(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  new (dest_p) FVector4(value_p->as<SkVector4>());
  return sizeof(FVector4);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_rotation_angles(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  new (dest_p) FRotator(value_p->as<SkRotationAngles>());
  return sizeof(FRotator);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_transform(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  new (dest_p) FTransform(value_p->as<SkTransform>());
  return sizeof(FTransform);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_struct_val(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  // Cast to uint32_t* hoping the compiler will get the hint and optimize the copy
  FMemory::Memcpy(reinterpret_cast<uint32_t *>(dest_p), reinterpret_cast<uint32_t *>(SkInstance::get_raw_pointer_val(value_p)), typed_name.m_byte_size);
  return typed_name.m_byte_size;
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_struct_ref(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  // Cast to uint32_t* hoping the compiler will get the hint and optimize the copy
  FMemory::Memcpy(reinterpret_cast<uint32_t *>(dest_p), reinterpret_cast<uint32_t *>(SkInstance::get_raw_pointer_ref(value_p)), typed_name.m_byte_size);
  return typed_name.m_byte_size;
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_entity(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  *((UObject **)dest_p) = value_p->as<SkUEEntity>();
  return sizeof(UObject *);
  }

//---------------------------------------------------------------------------------------

uint32_t SkUEReflectionManager::store_sk_value_enum(void * dest_p, SkInstance * value_p, const TypedName & typed_name)
  {
  *((UByteProperty::TCppType *)dest_p) = (UByteProperty::TCppType)value_p->as<SkEnum>();
  return sizeof(UByteProperty::TCppType);
  }

//---------------------------------------------------------------------------------------

#if 0 // Currently unused
void SkUEReflectionManager::ReflectedFunction::rebind_sk_invokable()
  {
  // Restore the invokable
  SkClass * sk_class_p = SkBrain::get_class(m_sk_class_name);
  SK_ASSERTX(sk_class_p, a_str_format("Could not find class `%s` while rebinding Blueprint exposed routines to new compiled binary.", m_sk_class_name.as_cstr()));
  if (sk_class_p)
    {
    SkInvokableBase * sk_invokable_p;
    if (m_is_class_member)
      {
      sk_invokable_p = sk_class_p->get_class_methods().get(get_name());
      }
    else
      {
      sk_invokable_p = sk_class_p->get_instance_methods().get(get_name());
      if (!sk_invokable_p)
        {
        sk_invokable_p = sk_class_p->get_coroutines().get(get_name());
        }
      }
    SK_ASSERTX(sk_invokable_p, a_str_format("Could not find routine `%s@%s` while rebinding Blueprint exposed routines to new compiled binary.", get_name_cstr(), m_sk_class_name.as_cstr()));
    if (m_type == ReflectedFunctionType_Event)
      {
      SkUEReflectionManager::bind_event_method(static_cast<SkMethodBase *>(sk_invokable_p));
      }
    m_sk_invokable_p = sk_invokable_p;
    }

  // Restore the parameter class pointers
  if (m_type == ReflectedFunctionType_Call)
    {
    ReflectedCallParam * param_array_p = static_cast<ReflectedCall *>(this)->get_param_array();
    for (uint32_t i = 0; i < m_num_params; ++i)
      {
      param_array_p[i].rebind_sk_class();
      }
    static_cast<ReflectedCall *>(this)->m_result_type.rebind_sk_class();
    }
  else
    {
    ReflectedEventParam * param_array_p = static_cast<ReflectedEvent *>(this)->get_param_array();
    for (uint32_t i = 0; i < m_num_params; ++i)
      {
      param_array_p[i].rebind_sk_class();
      }
    }
  }
#endif