#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSV_LINE_CAPACITY 96U
#define SREC_LINE_CAPACITY 520U
#define MAX_CSV_RECORDS 32U

static int line_was_complete(const char *line, FILE *stream)
{
    return strchr(line, '\n') != NULL || feof(stream) != 0;
}

static void trim_newline(char *line)
{
    size_t length = strlen(line);
    while (length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
        line[length - 1U] = '\0';
        --length;
    }
}

static int parse_u32_field(const char *text, int base, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long parsed = 0UL;

    if (text == NULL || *text == '\0' || out_value == NULL) {
        return 0;
    }
    errno = 0;
    parsed = strtoul(text, &end, base);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return 0;
    }
    *out_value = (uint32_t)parsed;
    return 1;
}

static int parse_csv_record(
    char *line,
    uint32_t *out_value,
    uint32_t *out_flags)
{
    char *first_comma = NULL;
    char *second_comma = NULL;
    uint32_t id = 0U;

    if (line == NULL || out_value == NULL || out_flags == NULL) {
        return 0;
    }
    first_comma = strchr(line, ',');
    if (first_comma == NULL) {
        return 0;
    }
    *first_comma = '\0';
    second_comma = strchr(first_comma + 1, ',');
    if (second_comma == NULL || strchr(second_comma + 1, ',') != NULL) {
        return 0;
    }
    *second_comma = '\0';

    return parse_u32_field(line, 10, &id) && id > 0U &&
           parse_u32_field(first_comma + 1, 10, out_value) &&
           parse_u32_field(second_comma + 1, 0, out_flags) &&
           *out_flags <= UINT32_C(0xFF);
}

static int process_csv(FILE *stream)
{
    char line[CSV_LINE_CAPACITY] = {0};
    size_t line_number = 0U;
    size_t records = 0U;
    uint32_t value_sum = 0U;
    uint32_t flags_or = 0U;

    if (fgets(line, sizeof line, stream) == NULL) {
        if (ferror(stream) != 0) {
            fputs("ERROR reading CSV file\n", stderr);
            return 3;
        }
        fputs("ERROR empty CSV file\n", stderr);
        return 2;
    }
    ++line_number;
    if (!line_was_complete(line, stream)) {
        fputs("ERROR CSV line too long at line 1\n", stderr);
        return 2;
    }
    trim_newline(line);
    if (strcmp(line, "id,value,flags") != 0) {
        fputs("ERROR CSV header must be id,value,flags\n", stderr);
        return 2;
    }

    while (fgets(line, sizeof line, stream) != NULL) {
        uint32_t value = 0U;
        uint32_t flags = 0U;

        ++line_number;
        if (!line_was_complete(line, stream)) {
            fprintf(stderr, "ERROR CSV line too long at line %zu\n", line_number);
            return 2;
        }
        trim_newline(line);
        if (records == MAX_CSV_RECORDS || !parse_csv_record(line, &value, &flags)) {
            fprintf(stderr, "ERROR invalid CSV record at line %zu\n", line_number);
            return 2;
        }
        if (UINT32_MAX - value_sum < value) {
            fprintf(stderr, "ERROR CSV value sum overflow at line %zu\n", line_number);
            return 2;
        }
        value_sum += value;
        flags_or |= flags;
        ++records;
    }
    if (ferror(stream) != 0) {
        fputs("ERROR reading CSV file\n", stderr);
        return 3;
    }
    if (records == 0U) {
        fputs("ERROR CSV has no records\n", stderr);
        return 2;
    }

    printf(
        "OK csv records=%zu value_sum=%" PRIu32 " flags_or=0x%02" PRIX32 "\n",
        records,
        value_sum,
        flags_or);
    return 0;
}

static int hex_nibble(char character, unsigned int *out_value)
{
    if (character >= '0' && character <= '9') {
        *out_value = (unsigned int)(character - '0');
        return 1;
    }
    if (character >= 'A' && character <= 'F') {
        *out_value = (unsigned int)(character - 'A') + 10U;
        return 1;
    }
    if (character >= 'a' && character <= 'f') {
        *out_value = (unsigned int)(character - 'a') + 10U;
        return 1;
    }
    return 0;
}

static int hex_byte(const char text[2], unsigned int *out_value)
{
    unsigned int high = 0U;
    unsigned int low = 0U;

    if (!hex_nibble(text[0], &high) || !hex_nibble(text[1], &low)) {
        return 0;
    }
    *out_value = (high << 4U) | low;
    return 1;
}

static int parse_srec_line(
    const char *line,
    size_t line_number,
    size_t *out_data_bytes,
    int *out_is_termination,
    uint16_t *out_start_address)
{
    unsigned int count = 0U;
    unsigned int sum = 0U;
    unsigned int byte_value = 0U;
    const char type = line[1];
    size_t address_bytes = 0U;
    size_t expected_length = 0U;
    uint32_t address = 0U;

    if (line[0] != 'S' || (type != '1' && type != '9') ||
        !hex_byte(&line[2], &count)) {
        fprintf(stderr, "ERROR invalid S-record syntax at line %zu\n", line_number);
        return 0;
    }
    address_bytes = 2U;
    if (count < address_bytes + 1U) {
        fprintf(stderr, "ERROR invalid S-record count at line %zu\n", line_number);
        return 0;
    }
    expected_length = 4U + (size_t)count * 2U;
    if (strlen(line) != expected_length) {
        fprintf(stderr, "ERROR invalid S-record length at line %zu\n", line_number);
        return 0;
    }

    sum = count;
    for (size_t byte_index = 0U; byte_index < (size_t)count; ++byte_index) {
        if (!hex_byte(&line[4U + byte_index * 2U], &byte_value)) {
            fprintf(stderr, "ERROR invalid S-record hex at line %zu\n", line_number);
            return 0;
        }
        sum = (sum + byte_value) & 0xFFU;
        if (byte_index < address_bytes) {
            address = (address << 8U) | byte_value;
        }
    }
    if (sum != 0xFFU) {
        fprintf(stderr, "ERROR S-record checksum mismatch at line %zu\n", line_number);
        return 0;
    }

    *out_data_bytes = (size_t)count - address_bytes - 1U;
    *out_is_termination = type == '9';
    *out_start_address = (uint16_t)address;
    if (type == '9' && *out_data_bytes != 0U) {
        fprintf(stderr, "ERROR S9 record contains data at line %zu\n", line_number);
        return 0;
    }
    return 1;
}

static int process_srec(FILE *stream)
{
    char line[SREC_LINE_CAPACITY] = {0};
    size_t line_number = 0U;
    size_t records = 0U;
    size_t data_bytes = 0U;
    uint16_t start_address = 0U;
    int saw_data_record = 0;
    int saw_termination = 0;

    while (fgets(line, sizeof line, stream) != NULL) {
        size_t record_data_bytes = 0U;
        int is_termination = 0;
        uint16_t record_address = 0U;

        ++line_number;
        if (!line_was_complete(line, stream)) {
            fprintf(stderr, "ERROR S-record line too long at line %zu\n", line_number);
            return 2;
        }
        trim_newline(line);
        if (saw_termination != 0 ||
            !parse_srec_line(
                line,
                line_number,
                &record_data_bytes,
                &is_termination,
                &record_address)) {
            if (saw_termination != 0) {
                fprintf(stderr, "ERROR record follows S9 termination at line %zu\n", line_number);
            }
            return 2;
        }
        if (is_termination != 0 && saw_data_record == 0) {
            fprintf(
                stderr,
                "ERROR S9 termination precedes S1 data at line %zu\n",
                line_number);
            return 2;
        }
        if (is_termination == 0 && record_data_bytes == 0U) {
            fprintf(stderr, "ERROR S1 record has no data at line %zu\n", line_number);
            return 2;
        }
        if (SIZE_MAX - data_bytes < record_data_bytes) {
            fputs("ERROR S-record data count overflow\n", stderr);
            return 2;
        }
        data_bytes += record_data_bytes;
        ++records;
        if (is_termination != 0) {
            saw_termination = 1;
            start_address = record_address;
        } else {
            saw_data_record = 1;
        }
    }
    if (ferror(stream) != 0) {
        fputs("ERROR reading S-record file\n", stderr);
        return 3;
    }
    if (records == 0U || saw_data_record == 0 || saw_termination == 0) {
        fputs("ERROR S-record profile requires S1 data plus S9 termination\n", stderr);
        return 2;
    }

    printf(
        "OK srec records=%zu data_bytes=%zu start=%04" PRIX16 "\n",
        records,
        data_bytes,
        start_address);
    return 0;
}

static int run_write_demo(const char *path)
{
    static const char payload[] = "status=ready\nrecords=2\n";
    char observed[sizeof payload] = {0};
    const size_t expected_length = sizeof payload - 1U;
    FILE *stream = NULL;
    size_t read_count = 0U;
    int written = 0;
    int extra = 0;

    stream = fopen(path, "wx");
    if (stream == NULL) {
        fprintf(stderr, "ERROR cannot open local output file: %s\n", path);
        return 3;
    }

    written = fprintf(stream, "%s", payload);
    if (written < 0 || (size_t)written != expected_length) {
        fputs("ERROR writing local output file\n", stderr);
        (void)fclose(stream);
        return 3;
    }
    if (fflush(stream) == EOF) {
        fputs("ERROR flushing local output file\n", stderr);
        (void)fclose(stream);
        return 3;
    }
    if (fclose(stream) == EOF) {
        fputs("ERROR closing local output file\n", stderr);
        return 3;
    }

    stream = fopen(path, "r");
    if (stream == NULL) {
        fputs("ERROR reopening local output file\n", stderr);
        return 3;
    }
    read_count = fread(observed, 1U, expected_length, stream);
    extra = fgetc(stream);
    if (ferror(stream) != 0) {
        fputs("ERROR reading local output file\n", stderr);
        (void)fclose(stream);
        return 3;
    }
    if (read_count != expected_length || extra != EOF ||
        memcmp(observed, payload, expected_length) != 0) {
        fputs("ERROR local output verification mismatch\n", stderr);
        (void)fclose(stream);
        return 3;
    }
    if (fclose(stream) == EOF) {
        fputs("ERROR closing verified local output file\n", stderr);
        return 3;
    }

    printf(
        "OK write-demo bytes=%zu flush=ok reopen=match\n",
        expected_length);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *stream = NULL;
    int status = 0;

    if (argc != 3 ||
        (strcmp(argv[1], "csv") != 0 && strcmp(argv[1], "srec") != 0 &&
         strcmp(argv[1], "write-demo") != 0)) {
        fprintf(stderr, "USAGE: %s <csv|srec|write-demo> <local-path>\n", argv[0]);
        return 64;
    }

    if (strcmp(argv[1], "write-demo") == 0) {
        return run_write_demo(argv[2]);
    }

    stream = fopen(argv[2], "r");
    if (stream == NULL) {
        fprintf(stderr, "ERROR cannot open local file: %s\n", argv[2]);
        return 3;
    }

    status = strcmp(argv[1], "csv") == 0 ? process_csv(stream) : process_srec(stream);
    if (fclose(stream) != 0 && status == 0) {
        fputs("ERROR closing local file\n", stderr);
        return 3;
    }
    return status;
}
