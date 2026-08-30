#pragma once

// Stand-in for lib/Serialization/PersistableStore.h. The real base drags in
// Arduino.h, Logging.h and the ArduinoJson serializer; none of that is needed
// to read CrossPointSettings' enum declarations, which is all this test does.
// getInstance() stays because CrossPointSettings' private constructor names
// PersistableStore<CrossPointSettings> as its friend.
template <typename T>
class PersistableStore {
 protected:
  PersistableStore() = default;
  ~PersistableStore() = default;

 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
  void requestResave() {}
};
