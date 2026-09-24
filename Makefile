PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=glue
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Extensions needed for testing: the S3 secret type lives in httpfs, the credential_chain provider in aws
CORE_EXTENSIONS='httpfs;parquet;aws;json'

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Local Glue (moto) + S3 (SeaweedFS) test servers, see scripts/docker-compose.yml (the compose file also creates the
# bucket and the Glue database 'default')
GLUE_COMPOSE=docker compose -f scripts/docker-compose.yml
glue-fixture:
	rm -f duckdb_benchmark_data/*.duckdb duckdb_benchmark_data/*.duckdb.wal
	$(GLUE_COMPOSE) up -d --wait
glue-fixture-down:
	$(GLUE_COMPOSE) down -v --remove-orphans
# Run the sqllogictests against the local servers (start them with `make glue-fixture` first), or against a live
# Glue catalog with the credentials of the AWS credential chain (see test/configs/cloud_glue.json)
# AWS_EC2_METADATA_DISABLED: the AWS SDK would otherwise ask the EC2 metadata service for a region (the test runner
# hides ~/.aws), which off EC2 hangs for minutes per client
test-local:
	AWS_EC2_METADATA_DISABLED=true ASAN_OPTIONS=detect_container_overflow=0 ./build/relassert/test/unittest --test-config test/configs/local_glue.json 'test/sql/*'
test-cloud:
	AWS_EC2_METADATA_DISABLED=true ASAN_OPTIONS=detect_container_overflow=0 ./build/relassert/test/unittest --test-config test/configs/cloud_glue.json 'test/sql/*'
