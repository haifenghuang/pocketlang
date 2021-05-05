/*
 *  Copyright (c) 2020-2021 Thakee Nathees
 *  Licensed under: MIT License
 */

#include <stdio.h>

#include "utils.h"
#include "var.h"
#include "vm.h"

// Public Api /////////////////////////////////////////////////////////////////
Var msVarBool(MSVM* vm, bool value) {
  return VAR_BOOL(value);
}

Var msVarNumber(MSVM* vm, double value) {
  return VAR_NUM(value);
}

Var msVarString(MSVM* vm, const char* value) {
  return VAR_OBJ(newString(vm, value, (uint32_t)strlen(value)));
}

bool msAsBool(MSVM* vm, Var value) {
  return AS_BOOL(value);
}

double msAsNumber(MSVM* vm, Var value) {
  return AS_NUM(value);
}

const char* msAsString(MSVM* vm, Var value) {
  return AS_STRING(value)->data;
}

///////////////////////////////////////////////////////////////////////////////

// Number of maximum digits for to_string buffer.
#define TO_STRING_BUFF_SIZE 128

// The maximum percentage of the map entries that can be filled before the map
// is grown. A lower percentage reduce collision which makes looks up faster
// but take more memory.
#define MAP_LOAD_PERCENT 75

// The factor a collection would grow by when it's exceeds the current capacity.
// The new capacity will be calculated by multiplying it's old capacity by the
// GROW_FACTOR.
#define GROW_FACTOR 2

void varInitObject(Object* self, MSVM* vm, ObjectType type) {
  self->type = type;
  self->is_marked = false;
  self->next = vm->first;
  vm->first = self;
}

void grayObject(Object* self, MSVM* vm) {
  if (self == NULL || self->is_marked) return;
  self->is_marked = true;

  // Add the object to the VM's gray_list so that we can recursively mark
  // it's referenced objects later.
  if (vm->gray_list_count >= vm->gray_list_capacity) {
    vm->gray_list_capacity *= 2;
    vm->gray_list = (Object**)vm->config.realloc_fn(
      vm->gray_list,
      vm->gray_list_capacity * sizeof(Object*),
      vm->config.user_data);
  }

  vm->gray_list[vm->gray_list_count++] = self;
}

void grayValue(Var self, MSVM* vm) {
  if (!IS_OBJ(self)) return;
  grayObject(AS_OBJ(self), vm);
}

void grayVarBuffer(VarBuffer* self, MSVM* vm) {
  for (uint32_t i = 0; i < self->count; i++) {
    grayValue(self->data[i], vm);
  }
}

void grayStringBuffer(StringBuffer* self, MSVM* vm) {
  for (uint32_t i = 0; i < self->count; i++) {
    grayObject((Object*)self->data[i], vm);
  }
}

void grayFunctionBuffer(FunctionBuffer* self, MSVM* vm) {
  for (uint32_t i = 0; i < self->count; i++) {
    grayObject((Object*)self->data[i], vm);
  }
}

void grayNameTable(NameTable* self, MSVM* vm) {
  for (uint32_t i = 0; i < self->count; i++) {
    grayObject((Object*)self->data[i], vm);
  }
}

static void blackenObject(Object* obj, MSVM* vm) {
  // TODO: trace here.

  switch (obj->type) {
    case OBJ_STRING: {
      vm->bytes_allocated += sizeof(String);
      vm->bytes_allocated += (size_t)(((String*)obj)->length + 1);
    } break;

    case OBJ_LIST: {
      List* list = (List*)obj;
      grayVarBuffer(&list->elements, vm);
      vm->bytes_allocated += sizeof(List);
      vm->bytes_allocated += sizeof(Var) * list->elements.capacity;
    } break;

    case OBJ_MAP: {
      Map* map = (Map*)obj;
      for (uint32_t i = 0; i < map->count; i++) {
        if (IS_UNDEF(map->entries[i].key)) continue;
        grayObject((Object*)map->entries[i].key, vm);
        grayObject((Object*)map->entries[i].value, vm);
      }
      vm->bytes_allocated += sizeof(Map);
      vm->bytes_allocated += sizeof(MapEntry) * map->capacity;
    } break;

    case OBJ_RANGE: {
      vm->bytes_allocated += sizeof(Range);
    } break;

    case OBJ_SCRIPT:
    {
      Script* script = (Script*)obj;
      vm->bytes_allocated += sizeof(Script);

      const int NT_ELEM_SIZE = sizeof(*script->global_names.data);

      grayVarBuffer(&script->globals, vm);
      vm->bytes_allocated += sizeof(Var) * script->globals.capacity;

      grayNameTable(&script->global_names, vm);
      vm->bytes_allocated += NT_ELEM_SIZE * script->global_names.capacity;

      grayVarBuffer(&script->literals, vm);
      vm->bytes_allocated += sizeof(Var) * script->literals.capacity;

      grayFunctionBuffer(&script->functions, vm);
      vm->bytes_allocated += sizeof(Function*) * script->functions.capacity;

      grayNameTable(&script->function_names, vm);
      vm->bytes_allocated += NT_ELEM_SIZE * script->function_names.capacity;

      grayStringBuffer(&script->names, vm);
      vm->bytes_allocated += sizeof(String*) * script->names.capacity;

      grayObject((Object*)script->body, vm);
    } break;

    case OBJ_FUNC:
    {
      Function* func = (Function*)obj;
      vm->bytes_allocated += sizeof(Function);

      grayObject((Object*)func->owner, vm);

      if (!func->is_native) {
        Fn* fn = func->fn;
        vm->bytes_allocated += sizeof(uint8_t)* fn->opcodes.capacity;
        vm->bytes_allocated += sizeof(int) * fn->oplines.capacity;
      }
    } break;

    case OBJ_FIBER:
    {
      Fiber* fiber = (Fiber*)obj;
      vm->bytes_allocated += sizeof(Fiber);

      grayObject((Object*)fiber->func, vm);

      // Blacken the stack.
      for (Var* local = fiber->stack; local < fiber->sp; local++) {
        grayValue(*local, vm);
      }
      vm->bytes_allocated += sizeof(Var) * fiber->stack_size;

      // Blacken call frames.
      for (int i = 0; i < fiber->frame_count; i++) {
        grayObject((Object*)fiber->frames[i].fn, vm);
        grayObject((Object*)fiber->frames[i].fn->owner, vm);
      }
      vm->bytes_allocated += sizeof(CallFrame) * fiber->frame_capacity;

      grayObject((Object*)fiber->error, vm);

    } break;

    case OBJ_USER:
      TODO;
      break;
  }
}


void blackenObjects(MSVM* vm) {
  while (vm->gray_list_count > 0) {
    // Pop the gray object from the list.
    Object* gray = vm->gray_list[--vm->gray_list_count];
    blackenObject(gray, vm);
  }
}

Var doubleToVar(double value) {
#if VAR_NAN_TAGGING
  return utilDoubleToBits(value);
#else
#error TODO:
#endif // VAR_NAN_TAGGING
}

double varToDouble(Var value) {
#if VAR_NAN_TAGGING
  return utilDoubleFromBits(value);
#else
  #error TODO:
#endif // VAR_NAN_TAGGING
}

static String* _allocateString(MSVM* vm, size_t length) {
  String* string = ALLOCATE_DYNAMIC(vm, String, length + 1, char);
  varInitObject(&string->_super, vm, OBJ_STRING);
  string->length = (uint32_t)length;
  string->data[length] = '\0';
  return string;
}

String* newString(MSVM* vm, const char* text, uint32_t length) {

  ASSERT(length == 0 || text != NULL, "Unexpected NULL string.");

  String* string = _allocateString(vm, length);

  if (length != 0 && text != NULL) memcpy(string->data, text, length);
  string->hash = utilHashString(string->data);

  return string;
}

List* newList(MSVM* vm, uint32_t size) {
  List* list = ALLOCATE(vm, List);
  varInitObject(&list->_super, vm, OBJ_LIST);
  varBufferInit(&list->elements);
  if (size > 0) {
    varBufferFill(&list->elements, vm, VAR_NULL, size);
    list->elements.count = 0;
  }
  return list;
}

Map* newMap(MSVM* vm) {
  Map* map = ALLOCATE(vm, Map);
  varInitObject(&map->_super, vm, OBJ_MAP);
  map->capacity = 0;
  map->count = 0;
  map->entries = NULL;
  return map;
}

Range* newRange(MSVM* vm, double from, double to) {
  Range* range = ALLOCATE(vm, Range);
  varInitObject(&range->_super, vm, OBJ_RANGE);
  range->from = from;
  range->to = to;
  return range;
}

Script* newScript(MSVM* vm) {
  Script* script = ALLOCATE(vm, Script);
  varInitObject(&script->_super, vm, OBJ_SCRIPT);

  script->name = NULL;
  // TODO: script->path = NULL;

  varBufferInit(&script->globals);
  nameTableInit(&script->global_names);
  varBufferInit(&script->literals);
  functionBufferInit(&script->functions);
  nameTableInit(&script->function_names);
  stringBufferInit(&script->names);

  vmPushTempRef(vm, &script->_super);
  const char* fn_name = "@(ScriptLevel)";
  script->body = newFunction(vm, fn_name, (int)strlen(fn_name), script, false);
  vmPopTempRef(vm);

  return script;
}

Function* newFunction(MSVM* vm, const char* name, int length, Script* owner,
                      bool is_native) {

  Function* func = ALLOCATE(vm, Function);
  varInitObject(&func->_super, vm, OBJ_FUNC);

  if (owner == NULL) {
    ASSERT(is_native, OOPS);
    func->name = name;
    func->owner = NULL;
    func->is_native = is_native;

  } else {
    // Add the name in the script's function buffer.
    String* name_ptr;
    vmPushTempRef(vm, &func->_super);
    functionBufferWrite(&owner->functions, vm, func);
    nameTableAdd(&owner->function_names, vm, name, length, &name_ptr);
    vmPopTempRef(vm);
  
    func->name = name_ptr->data;
    func->owner = owner;
    func->arity = -2; // -1 means variadic args.
    func->is_native = is_native;
  }


  if (is_native) {
    func->native = NULL;
  } else {
    Fn* fn = ALLOCATE(vm, Fn);

    byteBufferInit(&fn->opcodes);
    intBufferInit(&fn->oplines);
    fn->stack_size = 0;
    func->fn = fn;
  }
  return func;
}

Fiber* newFiber(MSVM* vm) {
  Fiber* fiber = ALLOCATE(vm, Fiber);
  memset(fiber, 0, sizeof(Fiber));
  varInitObject(&fiber->_super, vm, OBJ_FIBER);
  return fiber;
}

void listInsert(List* self, MSVM* vm, uint32_t index, Var value) {

  // Add an empty slot at the end of the buffer.
  if (IS_OBJ(value)) vmPushTempRef(vm, AS_OBJ(value));
  varBufferWrite(&self->elements, vm, VAR_NULL);
  if (IS_OBJ(value)) vmPopTempRef(vm);

  // Shift the existing elements down.
  for (uint32_t i = self->elements.count - 1; i > index; i--) {
    self->elements.data[i] = self->elements.data[i - 1];
  }

  // Insert the new element.
  self->elements.data[index] = value;
}

Var listRemoveAt(List* self, MSVM* vm, uint32_t index) {
  Var removed = self->elements.data[index];
  if (IS_OBJ(removed)) vmPushTempRef(vm, AS_OBJ(removed));

  // Shift the rest of the elements up.
  for (uint32_t i = index; i < self->elements.count - 1; i++) {
    self->elements.data[i] = self->elements.data[i + 1];
  }

  // Shrink the size if it's too much excess.
  if (self->elements.capacity / GROW_FACTOR >= self->elements.count) {
    self->elements.data = (Var*)vmRealloc(vm, self->elements.data,
      sizeof(Var) * self->elements.capacity,
      sizeof(Var) * self->elements.capacity / GROW_FACTOR);
    self->elements.capacity /= GROW_FACTOR;
  }

  if (IS_OBJ(removed)) vmPopTempRef(vm);

  self->elements.count--;
  return removed;
}

// Return a has value for the object.
static uint32_t _hashObject(Object* obj) {
  switch (obj->type) {

    case OBJ_STRING:
      return ((String*)obj)->hash;

    case OBJ_LIST:
    case OBJ_MAP:
      goto L_unhashable;

    case OBJ_RANGE:
    {
      Range* range = (Range*)obj;
      return utilHashNumber(range->from) ^ utilHashNumber(range->to);
    }

    case OBJ_SCRIPT:
    case OBJ_FUNC:
    case OBJ_FIBER:
    case OBJ_USER:
      TODO;

    default:
    L_unhashable:
      ASSERT(false, "Only immutable objects are hashable.");
      break;
  }
}

static uint32_t _hashVar(Var value) {
  if (IS_OBJ(value)) return _hashObject(AS_OBJ(value));

#if VAR_NAN_TAGGING
  return utilHashBits(value);
#else
  #error TODO:
#endif
}

// Find the entry with the [key]. Returns true if found and set [result] to
// point to the entry, return false otherwise and points [result] to where
// the entry should be inserted.
static bool _mapFindEntry(Map* self, Var key, MapEntry** result) {

  // An empty map won't contain the key.
  if (self->capacity == 0) return false;

  // The [start_index] is where the entry supposed to be if there wasn't any
  // collision occured. It'll be the start index for the linear probing.
  uint32_t start_index = _hashVar(key) % self->capacity;
  uint32_t index = start_index;

  // Keep track of the first tombstone after the [start_index] if we don't find
  // the key anywhere. The tombstone would be the entry at where we will have
  // to insert the key/value pair.
  MapEntry* tombstone = NULL;

  do {
    MapEntry* entry = &self->entries[index];

    if (IS_UNDEF(entry->key)) {
      ASSERT(IS_BOOL(entry->value), OOPS);

      if (IS_TRUE(entry->value)) {

        // We've found a tombstone, if we haven't found one [tombstone] should
        // be updated. We still need to keep search for if the key exists.
        if (tombstone == NULL) tombstone = entry;

      } else {
        // We've found a new empty slot and the key isn't found. If we've
        // found a tombstone along the sequence we could use that entry
        // otherwise the entry at the current index.

        *result = (tombstone != NULL) ? tombstone : entry;
        return false;
      }

    } else if (isValuesEqual(entry->key, key)) {
      // We've found the key.
      *result = entry;
      return true;
    }
    
    index = (index + 1) % self->capacity;

  } while (index != start_index);

  // If we reach here means the map is filled with tombstone. Set the first
  // tombstone as result for the next insertion and return false.
  ASSERT(tombstone != NULL, OOPS);
  *result = tombstone;
  return false;
}

// Add the key, value pair to the entries array of the map. Returns true if 
// the entry added for the first time and false for replaced vlaue.
static bool _mapInsertEntry(Map* self, Var key, Var value) {

  ASSERT(self->capacity != 0, "Should ensure the capacity before inserting.");

  MapEntry* result;
  if (_mapFindEntry(self, key, &result)) {
    // Key already found, just replace the value.
    result->value = value;
    return false;
  } else {
    result->key = key;
    result->value = value;
    return true;
  }
}

// Resize the map's size to the given [capacity].
static void _mapResize(Map* self, MSVM* vm, uint32_t capacity) {

  MapEntry* old_entries = self->entries;
  uint32_t old_capacity = self->capacity;

  self->entries = ALLOCATE_ARRAY(vm, MapEntry, capacity);
  self->capacity = capacity;
  for (uint32_t i = 0; i < capacity; i++) {
    self->entries->key = VAR_UNDEFINED;
    self->entries->value = VAR_FALSE;
  }

  // Insert the old entries to the new entries.
  for (uint32_t i = 0; i < old_capacity; i++) {
    // Skip the empty entries or tombstones.
    if (IS_UNDEF(old_entries[i].key)) continue;

    _mapInsertEntry(self, old_entries[i].key, old_entries[i].value);
  }

  DEALLOCATE(vm, old_entries);
}

Var mapGet(Map* self, Var key) {
  MapEntry* entry;
  if (_mapFindEntry(self, key, &entry)) return entry->value;
  return VAR_UNDEFINED;
}

void mapSet(Map* self, MSVM* vm, Var key, Var value) {

  // If map is about to fill, resize it first.
  if (self->count + 1 > self->capacity * MAP_LOAD_PERCENT / 100) {
    uint32_t capacity = self->capacity * GROW_FACTOR;
    if (capacity < MIN_CAPACITY) capacity = MIN_CAPACITY;
    _mapResize(self, vm, capacity);
  }

  if (_mapInsertEntry(self, key, value)) {
    self->count++; //< A new key added.
  }
}

void mapClear(Map* self, MSVM* vm) {
  DEALLOCATE(vm, self->entries);
  self->entries = NULL;
  self->capacity = 0;
  self->count = 0;
}

Var mapRemoveKey(Map* self, MSVM* vm, Var key) {
  MapEntry* entry;
  if (!_mapFindEntry(self, key, &entry)) return VAR_NULL;

  // Set the key as VAR_UNDEFINED to mark is as an available slow and set it's
  // value to VAR_TRUE for tombstone.
  Var value = entry->value;
  entry->key = VAR_UNDEFINED;
  entry->value = VAR_TRUE;

  self->count--;

  if (IS_OBJ(value)) vmPushTempRef(vm, AS_OBJ(value));

  if (self->count == 0) {
    // Clear the map if it's empty.
    mapClear(self, vm);

  } else if (self->capacity > MIN_CAPACITY &&
             self->capacity / GROW_FACTOR > self->count / MAP_LOAD_PERCENT * 100) {
    uint32_t capacity = self->capacity / GROW_FACTOR;
    if (capacity < MIN_CAPACITY) capacity = MIN_CAPACITY;

    _mapResize(self, vm, capacity);
  }

  if (IS_OBJ(value)) vmPopTempRef(vm);

  return value;
}

void freeObject(MSVM* vm, Object* obj) {
  // TODO: Debug trace memory here.

  // First clean the object's referencs, but we're not recursively doallocating
  // them because they're not marked and will be cleaned later.
  // Example: List's `elements` is VarBuffer that contain a heap allocated
  // array of `var*` which will be cleaned below but the actual `var` elements
  // will won't be freed here instead they havent marked at all, and will be
  // removed at the sweeping phase of the garbage collection.
  switch (obj->type) {
    case OBJ_STRING:
      break;

    case OBJ_LIST:
      varBufferClear(&(((List*)obj)->elements), vm);
      break;

    case OBJ_MAP:
      DEALLOCATE(vm, ((Map*)obj)->entries);
      break;

    case OBJ_RANGE:
      break;

    case OBJ_SCRIPT: {
      Script* scr = (Script*)obj;
      varBufferClear(&scr->globals, vm);
      nameTableClear(&scr->global_names, vm);
      varBufferClear(&scr->literals, vm);
      functionBufferClear(&scr->functions, vm);
      nameTableClear(&scr->function_names, vm);
      stringBufferClear(&scr->names, vm);
    } break;

    case OBJ_FUNC: {
      Function* func = (Function*)obj;
      if (!func->is_native) {
        byteBufferClear(&func->fn->opcodes, vm);
        intBufferClear(&func->fn->oplines, vm);
      }
    } break;

    case OBJ_FIBER: {
      Fiber* fiber = (Fiber*)obj;
      DEALLOCATE(vm, fiber->stack);
      DEALLOCATE(vm, fiber->frames);
    } break;

    case OBJ_USER:
      TODO; // Remove OBJ_USER.
      break;
  }

  DEALLOCATE(vm, obj);
}

// Utility functions //////////////////////////////////////////////////////////

const char* varTypeName(Var v) {
  if (IS_NULL(v)) return "null";
  if (IS_BOOL(v)) return "bool";
  if (IS_NUM(v))  return "number";

  ASSERT(IS_OBJ(v), OOPS);
  Object* obj = AS_OBJ(v);
  switch (obj->type) {
    case OBJ_STRING:  return "String";
    case OBJ_LIST:    return "List";
    case OBJ_MAP:     return "Map";
    case OBJ_RANGE:   return "Range";
    case OBJ_SCRIPT:  return "Script";
    case OBJ_FUNC:    return "Func";
    case OBJ_USER:    return "UserObj";
    default:
      UNREACHABLE();
  }
}

bool isValuesSame(Var v1, Var v2) {
#if VAR_NAN_TAGGING
  // Bit representation of each values are unique so just compare the bits.
  return v1 == v2;
#else
  #error TODO:
#endif
}

bool isValuesEqual(Var v1, Var v2) {
  if (isValuesSame(v1, v2)) return true;

  // If we reach here only heap allocated objects could be compared.
  if (!IS_OBJ(v1) || !IS_OBJ(v2)) return false;

  Object* o1 = AS_OBJ(v1), *o2 = AS_OBJ(v2);
  if (o1->type != o2->type) return false;

  switch (o1->type) {
    case OBJ_RANGE:
      return ((Range*)o1)->from == ((Range*)o2)->from &&
             ((Range*)o1)->to   == ((Range*)o2)->to;

    case OBJ_STRING: {
      String* s1 = (String*)o1, *s2 = (String*)o2;
      return s1->hash == s2->hash &&
             s1->length == s2->length &&
             memcmp(s1->data, s2->data, s1->length) == 0;
    }

    default:
      return false;
  }
}

String* toString(MSVM* vm, Var v, bool recursive) {

  if (IS_NULL(v)) {
    return newString(vm, "null", 4);

  } else if (IS_BOOL(v)) {
    if (AS_BOOL(v)) {
      return newString(vm, "true", 4);
    } else {
      return newString(vm, "false", 5);
    }

  } else if (IS_NUM(v)) {
    char buff[TO_STRING_BUFF_SIZE];
    int length = sprintf(buff, "%.14g", AS_NUM(v));
    ASSERT(length < TO_STRING_BUFF_SIZE, "Buffer overflowed.");
    return newString(vm, buff, length);

  } else if (IS_OBJ(v)) {
    Object* obj = AS_OBJ(v);
    switch (obj->type) {
      case OBJ_STRING:
      {
        // If recursive return with quotes (ex: [42, "hello", 0..10]).
        String* string = newString(vm, ((String*)obj)->data, ((String*)obj)->length);
        if (!recursive) return string;
        else return (String*)AS_OBJ(stringFormat(vm, "\"@\"", string));
      }

      case OBJ_LIST: {
        List* list = (List*)obj;
        String* result = newString(vm, "[", 1);

        for (uint32_t i = 0; i < list->elements.count; i++) {
          const char* fmt = (i != 0) ? "@, @" : "@@";

          vmPushTempRef(vm, &result->_super);
          result = (String*)AS_OBJ(stringFormat(vm, fmt,
            result, toString(vm, list->elements.data[i], true)));
          vmPopTempRef(vm);
        }
        return (String*)AS_OBJ(stringFormat(vm, "@]", result));
      }

      case OBJ_MAP:    return newString(vm, "[Map]",     5); // TODO;
      case OBJ_RANGE:  return newString(vm, "[Range]",   7); // TODO;
      case OBJ_SCRIPT: return newString(vm, "[Script]",  8); // TODO;
      case OBJ_FUNC: {
        const char* name = ((Function*)obj)->name;
        int length = (int)strlen(name); // TODO: Assert length.
        char buff[TO_STRING_BUFF_SIZE];
        memcpy(buff, "[Func:", 6);
        memcpy(buff + 6, name, length);
        buff[6 + length] = ']';
        return newString(vm, buff, 6 + length + 1);
      }
      case OBJ_USER:   return newString(vm, "[UserObj]", 9); // TODO;
        break;
    }

  }
  UNREACHABLE();
  return NULL;
}

bool toBool(Var v) {

  if (IS_BOOL(v)) return AS_BOOL(v);
  if (IS_NULL(v)) return false;
  if (IS_NUM(v)) return AS_NUM(v) != 0;

  ASSERT(IS_OBJ(v), OOPS);
  Object* o = AS_OBJ(v);
  switch (o->type) {
    case OBJ_STRING: return ((String*)o)->length != 0;
    case OBJ_LIST:   return ((List*)o)->elements.count != 0;
    case OBJ_MAP:    return ((Map*)o)->count != 0;
    case OBJ_RANGE: // [[FALLTHROUGH]]
    case OBJ_SCRIPT:
    case OBJ_FUNC:
    case OBJ_USER:
      return true;
    default:
      UNREACHABLE();
  }

  return true;
}

Var stringFormat(MSVM* vm, const char* fmt, ...) {
  va_list arg_list;

  // Calculate the total length of the resulting string. This is required to 
  // determine the final string size to allocate.
  va_start(arg_list, fmt);
  size_t total_length = 0;
  for (const char* c = fmt; *c != '\0'; c++) {
    switch (*c) {
      case '$':
        total_length += strlen(va_arg(arg_list, const char*));
        break;

      case '@':
        total_length += AS_STRING(va_arg(arg_list, Var))->length;
        break;

      default:
        total_length++;
    }
  }
  va_end(arg_list);

  // Now build the new string.
  String* result = _allocateString(vm, total_length);
  va_start(arg_list, fmt);
  char* buff = result->data;
  for (const char* c = fmt; *c != '\0'; c++) {
    switch (*c) {
      case '$':
      {
        const char* string = va_arg(arg_list, const char*);
        size_t length = strlen(string);
        memcpy(buff, string, length);
        buff += length;
      } break;

      case '@':
      {
        String* string = AS_STRING(va_arg(arg_list, Var));
        memcpy(buff, string->data, string->length);
        buff += string->length;
      } break;

      default:
      {
        *buff++ = *c;
      } break;
    }
  }
  va_end(arg_list);

  result->hash = utilHashString(result->data);
  return VAR_OBJ(result);
}