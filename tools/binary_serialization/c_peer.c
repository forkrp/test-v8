/* Independent c_master decoder/encoder interoperability check. */
#include <msgpack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  FILE* file = fopen(argv[1], "rb");
  if (!file || fseek(file, 0, SEEK_END)) return 2;
  long length = ftell(file);
  if (length <= 0 || fseek(file, 0, SEEK_SET)) return 2;
  char* bytes = malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length)
    return 2;
  fclose(file);
  msgpack_unpacked unpacked;
  msgpack_unpacked_init(&unpacked);
  size_t offset = 0;
  msgpack_unpack_return status =
      msgpack_unpack_next(&unpacked, bytes, (size_t)length, &offset);
  if (status != MSGPACK_UNPACK_SUCCESS || offset != (size_t)length) return 1;
  msgpack_sbuffer buffer;
  msgpack_sbuffer_init(&buffer);
  msgpack_packer packer;
  msgpack_packer_init(&packer, &buffer, msgpack_sbuffer_write);
  if (msgpack_pack_object(&packer, unpacked.data) ||
      buffer.size != (size_t)length || memcmp(buffer.data, bytes, buffer.size))
    return 1;
  printf("{\"cMasterWireRoundTrip\":\"PASS\",\"bytes\":%zu}\n", buffer.size);
  msgpack_sbuffer_destroy(&buffer);
  msgpack_unpacked_destroy(&unpacked);
  free(bytes);
  return 0;
}
