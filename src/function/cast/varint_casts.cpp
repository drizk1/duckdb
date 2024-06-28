#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {
constexpr uint8_t VARINT_HEADER_SIZE = 3;

string_t IntToVarInt(int32_t int_value) {
	// Determine if the number is negative
	bool is_negative = int_value < 0;

	// If negative, convert to two's complement
	if (is_negative) {
		int_value = ~int_value;
	}

	// Determine the number of data bytes
	uint64_t abs_value = std::abs(int_value);
	uint32_t data_byte_size = (abs_value == 0) ? 1 : static_cast<int>(std::ceil(std::log2(abs_value + 1) / 8.0));

	// Create the header
	uint32_t header = data_byte_size;
	if (!is_negative) {
		header = ~header;
	}

	string_t blob {data_byte_size + VARINT_HEADER_SIZE};

	auto writable_blob = blob.GetDataWriteable();
	// Add header bytes to the blob
	idx_t wb_idx = 0;
	writable_blob[wb_idx++] = static_cast<uint8_t>(header >> 16);
	writable_blob[wb_idx++] = static_cast<uint8_t>((header >> 8) & 0xFF);
	writable_blob[wb_idx++] = static_cast<uint8_t>((header >> 8) & 0xFF);

	// Add data bytes to the blob
	for (int i = data_byte_size - 1; i >= 0; --i) {
		writable_blob[wb_idx++] = static_cast<uint8_t>((int_value >> (i * 8)) & 0xFF);
	}
	return blob;
}

int CharToDigit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	throw InvalidInputException("bad string");
}

string_t VarcharToVarInt(string_t int_value) {
	if (int_value.Empty()) {
		throw InvalidInputException("bad string");
	}

	auto int_value_char = int_value.GetData();
	idx_t int_value_size = int_value.GetSize();
	idx_t start_pos = 0;

	// check if first character is -
	bool is_negative = int_value_char[0] == '-';
	if (is_negative) {
		start_pos++;
	}
	// trim 0's, unless value is 0
	if (int_value_size - start_pos > 1) {
		while (int_value_char[start_pos] == '0' && start_pos < int_value_size) {
			start_pos++;
		}
	}
	idx_t actual_size = int_value_size - start_pos;
	uint32_t data_byte_size = ceil(actual_size * log2(10) / 8);
	string_t blob {data_byte_size + VARINT_HEADER_SIZE};

	uint32_t header = data_byte_size;
	if (!is_negative) {
		header = ~header;
	}

	auto writable_blob = blob.GetDataWriteable();
	// Add header bytes to the blob

	writable_blob[0] = static_cast<uint8_t>(header >> 16);
	writable_blob[1] = static_cast<uint8_t>((header >> 8) & 0xFF);
	writable_blob[2] = static_cast<uint8_t>((header >> 8) & 0xFF);

	// convert the string to a byte array
	string abs_str(int_value_char + start_pos, actual_size);
	idx_t wb_idx = data_byte_size + VARINT_HEADER_SIZE - 1;
	while (!abs_str.empty()) {
		uint8_t remainder = 0;
		std::string quotient;
		// We convert ze string to a big-endian byte array by dividing the number by 256 and storing the remainders
		for (char digit : abs_str) {
			int new_value = remainder * 10 + CharToDigit(digit);
			quotient += (new_value / 256) + '0';
			remainder = new_value % 256;
		}
		if (is_negative) {
			writable_blob[wb_idx--] = ~remainder;
		} else {
			writable_blob[wb_idx--] = remainder;
		}

		// Remove leading zeros from the quotient
		abs_str = quotient.erase(0, quotient.find_first_not_of('0'));
	}
	return blob;
}

// Function to convert VARINT blob to a VARCHAR
// FIXME: This should probably use a double
string_t VarIntToVarchar(string_t &blob) {
	if (blob.GetSize() < 4) {
		throw std::invalid_argument("Invalid blob size.");
	}
	auto blob_ptr = blob.GetData();

	// Extract the header
	uint32_t header = (blob_ptr[0] << 16) | (blob_ptr[1] << 8) | blob_ptr[2];

	// Determine the number of data bytes
	int data_byte_size = blob.GetSize() - 3;

	// Determine if the number is negative
	bool is_negative = (header & (1 << 23)) == 0;

	// Extract the data bytes
	int64_t int_value = 0;
	for (int i = 0; i < data_byte_size; ++i) {
		int_value = (int_value << 8) | blob_ptr[3 + i];
	}

	// If negative, convert from two's complement
	if (is_negative) {
		int_value = ~int_value;
	}

	return std::to_string(int_value);
}

struct IntTryCastToVarInt {
	template <class SRC>
	static inline string_t Operation(SRC input, Vector &result) {
		return StringVector::AddStringOrBlob(result, IntToVarInt(input));
	}
};

struct VarcharTryCastToVarInt {
	template <class SRC>
	static inline string_t Operation(SRC input, Vector &result) {
		return StringVector::AddStringOrBlob(result, VarcharToVarInt(input));
	}
};

struct VarIntTryCastToVarchar {
	template <class SRC>
	static inline string_t Operation(SRC input, Vector &result) {
		return StringVector::AddStringOrBlob(result, VarIntToVarchar(input));
	}
};

BoundCastInfo DefaultCasts::ToVarintCastSwitch(BindCastInput &input, const LogicalType &source,
                                               const LogicalType &target) {
	D_ASSERT(target.id() == LogicalTypeId::VARINT);
	// now switch on the result type
	switch (source.id()) {
	case LogicalTypeId::INTEGER:
		return BoundCastInfo(&VectorCastHelpers::StringCast<int32_t, duckdb::IntTryCastToVarInt>);
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<string_t, duckdb::VarcharTryCastToVarInt>);
	case LogicalTypeId::DOUBLE:
		return TryVectorNullCast;
	default:
		return TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::VarintCastSwitch(BindCastInput &input, const LogicalType &source,
                                             const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::VARINT);
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<string_t, duckdb::VarIntTryCastToVarchar>);
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
