/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ORC_TYPE_HH
#define ORC_TYPE_HH

#include "MemoryPool.hh"
#include "orc/Vector.hh"
#include "orc/orc-config.hh"

namespace orc {

  namespace geospatial {
    enum EdgeInterpolationAlgorithm {
      SPHERICAL = 0,
      VINCENTY = 1,
      THOMAS = 2,
      ANDOYER = 3,
      KARNEY = 4
    };
    std::string AlgoToString(EdgeInterpolationAlgorithm algo);
    EdgeInterpolationAlgorithm AlgoFromString(const std::string& algo);
  }  // namespace geospatial

  enum TypeKind {
    BOOLEAN = 0,
    BYTE = 1,
    SHORT = 2,
    INT = 3,
    LONG = 4,
    FLOAT = 5,
    DOUBLE = 6,
    STRING = 7,
    BINARY = 8,
    TIMESTAMP = 9,
    LIST = 10,
    MAP = 11,
    STRUCT = 12,
    UNION = 13,
    DECIMAL = 14,
    DATE = 15,
    VARCHAR = 16,
    CHAR = 17,
    TIMESTAMP_INSTANT = 18,
    GEOMETRY = 19,
    GEOGRAPHY = 20
  };

  class Type {
   public:
    virtual ~Type();
    virtual uint64_t getColumnId() const = 0;
    virtual uint64_t getMaximumColumnId() const = 0;
    virtual TypeKind getKind() const = 0;
    virtual uint64_t getSubtypeCount() const = 0;
    virtual const Type* getSubtype(uint64_t childId) const = 0;
    virtual const std::string& getFieldName(uint64_t childId) const = 0;
    virtual uint64_t getMaximumLength() const = 0;
    virtual uint64_t getPrecision() const = 0;
    virtual uint64_t getScale() const = 0;
    // for geospatial types only
    virtual const std::string& getCrs() const = 0;
    // for geography type only
    virtual geospatial::EdgeInterpolationAlgorithm getAlgorithm() const = 0;
    virtual Type& setAttribute(const std::string& key, const std::string& value) = 0;
    virtual bool hasAttributeKey(const std::string& key) const = 0;
    virtual Type& removeAttribute(const std::string& key) = 0;
    virtual std::vector<std::string> getAttributeKeys() const = 0;
    virtual std::string getAttributeValue(const std::string& key) const = 0;
    virtual std::string toString() const = 0;
    /**
     * Get the Type with the given column ID
     * @param colId the column ID
     * @return the type corresponding to the column Id, nullptr if not exists
     */
    virtual const Type* getTypeByColumnId(uint64_t colId) const = 0;

    /**
     * Create a row batch for this type.
     */
    virtual std::unique_ptr<ColumnVectorBatch> createRowBatch(uint64_t size, MemoryPool& pool,
                                                              bool encoded = false) const = 0;

    virtual std::unique_ptr<ColumnVectorBatch> createRowBatch(uint64_t size, MemoryPool& pool,
                                                              bool encoded,
                                                              bool useTightNumericVector) const = 0;

    /**
     * Add a new field to a struct type.
     * @param fieldName the name of the new field
     * @param fieldType the type of the new field
     * @return a reference to the struct type
     */
    virtual Type* addStructField(const std::string& fieldName, std::unique_ptr<Type> fieldType) = 0;

    /**
     * Add a new child to a union type.
     * @param fieldType the type of the new field
     * @return a reference to the union type
     */
    virtual Type* addUnionChild(std::unique_ptr<Type> fieldType) = 0;

    /**
     * Build a Type object from string text representation.
     */
    static std::unique_ptr<Type> buildTypeFromString(const std::string& input);
  };

  const int64_t DEFAULT_DECIMAL_SCALE = 18;
  const int64_t DEFAULT_DECIMAL_PRECISION = 38;

  std::unique_ptr<Type> createPrimitiveType(TypeKind kind);
  std::unique_ptr<Type> createCharType(TypeKind kind, uint64_t maxLength);
  std::unique_ptr<Type> createDecimalType(uint64_t precision = DEFAULT_DECIMAL_PRECISION,
                                          uint64_t scale = DEFAULT_DECIMAL_SCALE);

  std::unique_ptr<Type> createStructType();
  std::unique_ptr<Type> createListType(std::unique_ptr<Type> elements);
  std::unique_ptr<Type> createMapType(std::unique_ptr<Type> key, std::unique_ptr<Type> value);
  std::unique_ptr<Type> createUnionType();
  std::unique_ptr<Type> createGeometryType(const std::string& crs = "OGC:CRS84");
  std::unique_ptr<Type> createGeographyType(
      const std::string& crs = "OGC:CRS84",
      geospatial::EdgeInterpolationAlgorithm algo = geospatial::SPHERICAL);

}  // namespace orc
#endif
