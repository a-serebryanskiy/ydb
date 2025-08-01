#!/usr/bin/env python
# -*- coding: utf-8 -*-

import boto3
import logging

import pytest
import time

import ydb.public.api.protos.ydb_value_pb2 as ydb
import ydb.public.api.protos.draft.fq_pb2 as fq

import ydb.tests.fq.s3.s3_helpers as s3_helpers
from ydb.tests.tools.fq_runner.kikimr_utils import yq_v1, yq_all

from moto import __version__ as moto_version
from packaging.version import Version


class TestS3(object):
    def create_bucket_and_upload_file(self, filename, s3, kikimr):
        s3_helpers.create_bucket_and_upload_file(filename, s3.s3_url, "fbucket", "ydb/tests/fq/s3/test_format_data")
        kikimr.control_plane.wait_bootstrap(1)

    @yq_all
    @pytest.mark.parametrize("dataset_name", ["dataset", "dataにちは% set"])
    @pytest.mark.parametrize("format", ["json_list", "json_each_row", "csv_with_names", "parquet"])
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    @pytest.mark.parametrize("block_sink", ["false", "true"])
    def test_insert(self, kikimr, s3, client, format, dataset_name, unique_prefix, block_sink):
        if block_sink == "true" and format != "parquet":
            pytest.skip(f"block sink is not supported for format {format}")
            return

        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = f'''
            pragma s3.UseBlocksSink = "{block_sink}";
            insert into `{storage_connection_name}`.`{dataset_name}/` with (format={format})
            select * from AS_TABLE([<|foo:123, bar:"xxx"u|>,<|foo:456, bar:"yyy"u|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        prefix = client.describe_query(query_id).result.query.meta.last_job_id.split("-")[0]  # cut _<query_id> part

        sql = f'''
            select foo, bar from {storage_connection_name}.`{dataset_name}/{prefix}*` with (format={format}, schema(
                foo Int NOT NULL,
                bar String NOT NULL
            ))
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 2
        assert result_set.columns[0].name == "foo"
        assert result_set.columns[0].type.type_id == ydb.Type.INT32
        assert result_set.columns[1].name == "bar"
        assert result_set.columns[1].type.type_id == ydb.Type.STRING
        assert len(result_set.rows) == 2
        assert result_set.rows[0].items[0].int32_value == 123
        assert result_set.rows[0].items[1].bytes_value == b'xxx'
        assert result_set.rows[1].items[0].int32_value == 456
        assert result_set.rows[1].items[1].bytes_value == b'yyy'
        assert sum(kikimr.control_plane.get_metering(1)) == 20

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_big_json_list_insert(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("big_data_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        s3_client = boto3.client(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        taxi = '''VendorID'''
        for i in range(37):
            taxi += "\n" + str(i)
        s3_client.put_object(Body=taxi, Bucket='big_data_bucket', Key='src/taxi.csv', ContentType='text/plain')

        connection_response = client.create_storage_connection(unique_prefix + "big_data_bucket", "big_data_bucket")

        vendorID = ydb.Column(
            name="VendorID",
            type=ydb.Type(optional_type=ydb.OptionalType(item=ydb.Type(type_id=ydb.Type.PrimitiveTypeId.STRING))),
        )
        storage_source_binding_name = unique_prefix + "taxi_src_csv_with_names"
        client.create_object_storage_binding(
            name=storage_source_binding_name,
            path="src/",
            format="csv_with_names",
            connection_id=connection_response.result.connection_id,
            columns=[vendorID],
        )

        storage_sink_binding_name = unique_prefix + "taxi_dst_json_list_zstd"
        client.create_object_storage_binding(
            name=storage_sink_binding_name,
            path="dst/",
            format="json_list",
            compression="zstd",
            connection_id=connection_response.result.connection_id,
            columns=[vendorID],
        )

        client.create_storage_connection("ibucket", "insert_bucket")

        sql = f'''
            pragma s3.JsonListSizeLimit="10";
            INSERT INTO bindings.`{storage_sink_binding_name}`
            SELECT
                VendorID
            FROM bindings.`{storage_source_binding_name}`
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''

            SELECT
                count(*)
            FROM bindings.`{storage_sink_binding_name}`
        '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 1
        assert result_set.columns[0].name == "column0"
        assert result_set.columns[0].type.type_id == ydb.Type.UINT64
        assert len(result_set.rows) == 1
        assert result_set.rows[0].items[0].uint64_value == 37
        assert sum(kikimr.control_plane.get_metering(1)) == 20

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_insert_csv_delimiter(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = f'''
            insert into `{storage_connection_name}`.`csv_delim_out/` with (
              format=csv_with_names,
              csv_delimiter=";"
            )
            select * from AS_TABLE([<|foo:123, bar:"xxx"u|>,<|foo:456, bar:"yyy"u|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        prefix = (
            ""  # client.describe_query(query_id).result.query.meta.last_job_id.split("-")[0]  # cut _<query_id> part
        )

        sql = f'''
            select data from `{storage_connection_name}`.`csv_delim_out/{prefix}*` with (format=raw, schema(
                data String NOT NULL
            ))
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 1
        assert result_set.columns[0].name == "data"
        assert result_set.columns[0].type.type_id == ydb.Type.STRING
        assert len(result_set.rows) == 1
        assert result_set.rows[0].items[0].bytes_value == b'"bar";"foo"\n"xxx";123\n"yyy";456\n'
        assert sum(kikimr.control_plane.get_metering(1)) == 20

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_append(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("append_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "abucket"
        client.create_storage_connection(storage_connection_name, "append_bucket")

        sql = f'''
            insert into `{storage_connection_name}`.`append/` with (format=json_each_row)
            select * from AS_TABLE([<|foo:123, bar:"xxx"u|>,<|foo:456, bar:"yyy"u|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            insert into `{storage_connection_name}`.`append/` with (format=json_each_row)
            select * from AS_TABLE([<|foo:345, bar:"zzz"u|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            select foo, bar from `{storage_connection_name}`.`append/` with (format=json_each_row, schema(
                foo Int NOT NULL,
                bar String NOT NULL
            )) order by foo
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 2
        assert result_set.columns[0].name == "foo"
        assert result_set.columns[0].type.type_id == ydb.Type.INT32
        assert result_set.columns[1].name == "bar"
        assert result_set.columns[1].type.type_id == ydb.Type.STRING
        assert len(result_set.rows) == 3
        assert result_set.rows[0].items[0].int32_value == 123
        assert result_set.rows[0].items[1].bytes_value == b'xxx'
        assert result_set.rows[1].items[0].int32_value == 345
        assert result_set.rows[1].items[1].bytes_value == b'zzz'
        assert result_set.rows[2].items[0].int32_value == 456
        assert result_set.rows[2].items[1].bytes_value == b'yyy'
        assert sum(kikimr.control_plane.get_metering(1)) == 30

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_part_split(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("split_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "sbucket"
        client.create_storage_connection(storage_connection_name, "split_bucket")

        sql = f'''
            insert into `{storage_connection_name}`.`part/` with (format=json_each_row, partitioned_by=(foo, bar))
            select * from AS_TABLE([<|foo:123, bar:"xxx"u, data:3.14|>,<|foo:456, bar:"yyy"u, data:2.72|>,<|foo:123, bar:"xxx"u, data:1.41|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            select data from `{storage_connection_name}`.`part/foo=123/bar=xxx/` with (format=json_each_row, schema(
                data Float NOT NULL,
            ))
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 1
        assert result_set.columns[0].name == "data"
        assert result_set.columns[0].type.type_id == ydb.Type.FLOAT
        assert len(result_set.rows) == 2
        assert abs(result_set.rows[0].items[0].float_value - 3.14) < 0.01
        assert abs(result_set.rows[1].items[0].float_value - 1.41) < 0.01
        assert sum(kikimr.control_plane.get_metering(1)) == 20

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_part_merge(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("merge_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "mbucket"
        client.create_storage_connection(storage_connection_name, "merge_bucket")

        sql = f'''
            insert into `{storage_connection_name}`.`part/foo=123/bar=xxx/` with (format=json_each_row)
            select * from AS_TABLE([<|data:3.14|>,<|data:1.41|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            insert into `{storage_connection_name}`.`part/foo=456/bar=yyy/` with (format=json_each_row)
            select * from AS_TABLE([<|data:2.72|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            select foo, bar, data from `{storage_connection_name}`.`part` with (format=json_each_row, partitioned_by=(foo, bar), schema(
                foo Int NOT NULL,
                bar String NOT NULL,
                data Float NOT NULL
            )) order by foo, data
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 3
        assert result_set.columns[0].name == "foo"
        assert result_set.columns[0].type.type_id == ydb.Type.INT32
        assert result_set.columns[1].name == "bar"
        assert result_set.columns[1].type.type_id == ydb.Type.STRING
        assert result_set.columns[2].name == "data"
        assert result_set.columns[2].type.type_id == ydb.Type.FLOAT
        assert len(result_set.rows) == 3
        assert result_set.rows[0].items[0].int32_value == 123
        assert result_set.rows[0].items[1].bytes_value == b'xxx'
        assert abs(result_set.rows[0].items[2].float_value - 1.41) < 0.01
        assert result_set.rows[1].items[0].int32_value == 123
        assert result_set.rows[1].items[1].bytes_value == b'xxx'
        assert abs(result_set.rows[1].items[2].float_value - 3.14) < 0.01
        assert result_set.rows[2].items[0].int32_value == 456
        assert result_set.rows[2].items[1].bytes_value == b'yyy'
        assert abs(result_set.rows[2].items[2].float_value - 2.72) < 0.01
        assert sum(kikimr.control_plane.get_metering(1)) == 30

    @yq_all
    @pytest.mark.parametrize("format", ["json_list", "json_each_row", "csv_with_names"])
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_part_binding(self, kikimr, s3, client, format, unique_prefix):
        if format == "json_list":
            pytest.skip("json_list does not work with partitioned_by. YQ-1335")
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("binding_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        connection_response = client.create_storage_connection(unique_prefix + "bbucket", "binding_bucket")

        fooType = ydb.Column(name="foo", type=ydb.Type(type_id=ydb.Type.PrimitiveTypeId.INT32))
        barType = ydb.Column(name="bar", type=ydb.Type(type_id=ydb.Type.PrimitiveTypeId.UTF8))
        dataType = ydb.Column(name="data", type=ydb.Type(type_id=ydb.Type.PrimitiveTypeId.DOUBLE))

        storage_binding_name = unique_prefix + "bbinding"
        client.create_object_storage_binding(
            name=storage_binding_name,
            path=format + "/",
            format=format,
            connection_id=connection_response.result.connection_id,
            columns=[fooType, barType, dataType],
            partitioned_by=["foo", "bar"],
            format_setting={"file_pattern": "*{json,csv}"},
        )

        sql = f'''
            insert into bindings.`{storage_binding_name}`
            select * from AS_TABLE([<|foo:123, bar:"xxx"u, data:3.14|>,<|foo:456, bar:"yyy"u, data:2.72|>,<|foo:123, bar:"xxx"u, data:1.41|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            select foo, bar, data from bindings.`{storage_binding_name}` order by foo, data
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 3
        assert result_set.columns[0].name == "foo"
        assert result_set.columns[0].type.type_id == ydb.Type.INT32
        assert result_set.columns[1].name == "bar"
        assert result_set.columns[1].type.type_id == ydb.Type.UTF8
        assert result_set.columns[2].name == "data"
        assert result_set.columns[2].type.type_id == ydb.Type.DOUBLE
        assert len(result_set.rows) == 3
        assert result_set.rows[0].items[0].int32_value == 123
        assert result_set.rows[0].items[1].text_value == 'xxx'
        assert abs(result_set.rows[0].items[2].double_value - 1.41) < 0.01
        assert result_set.rows[1].items[0].int32_value == 123
        assert result_set.rows[1].items[1].text_value == 'xxx'
        assert abs(result_set.rows[1].items[2].double_value - 3.14) < 0.01
        assert result_set.rows[2].items[0].int32_value == 456
        assert result_set.rows[2].items[1].text_value == 'yyy'
        assert abs(result_set.rows[2].items[2].double_value - 2.72) < 0.01
        assert sum(kikimr.control_plane.get_metering(1)) == 20

    @yq_v1
    @pytest.mark.parametrize("format", ["json_each_row", "csv_with_names", "tsv_with_names", "parquet"])
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_error(self, kikimr, s3, client, format, unique_prefix):
        if format == "parquet":
            pytest.skip("Transient errors do not work for arrow reader - YQ-1335")
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("error_bucket")
        acl = '' if Version(moto_version) < Version("4.0.5") else 'public-write'
        bucket.create(ACL=acl)
        bucket.objects.all().delete()

        kikimr.control_plane.wait_bootstrap(1)
        storage_connection_name = unique_prefix + "ebucket"
        client.create_storage_connection(storage_connection_name, "error_bucket")

        sql = R'''
            insert into `{1}`.`{0}/` with (format={0})
            select * from AS_TABLE([<|foo:123, bar:"xxx"u|>,<|foo:456, bar:"yyy"u|>]);
            '''.format(
            format, storage_connection_name
        )

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        sql = R'''
            select foo, bar from `{1}`.`{0}/` with (format={0}, schema(
                foo Int NOT NULL,
                bar String NOT NULL
            ))
            '''.format(
            format, storage_connection_name
        )

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        if Version(moto_version) < Version("4.0.5"):
            start_at = time.time()
            while True:
                result = client.describe_query(query_id).result
                assert result.query.meta.status in [
                    fq.QueryMeta.STARTING,
                    fq.QueryMeta.RUNNING,
                ], "Query is not RUNNING anymore"
                issues = result.query.transient_issue
                if "500 Internal Server Error" in str(issues):
                    break
                assert time.time() - start_at < 20, "Timeout waiting for transient issue in " + str(issues)
                time.sleep(0.5)
        else:
            # Available since moto version 4.0.5+
            client.wait_query_status(query_id, fq.QueryMeta.FAILED)
            msg = client.describe_query(query_id).result.query.issue
            assert 'HTTP error code: 403' in str(msg)
            assert 'Query failed with code EXTERNAL_ERROR' in str(msg)

        client.abort_query(query_id)
        client.wait_query(query_id)

    @yq_all
    def test_insert_empty_object(self, kikimr, s3, client, unique_prefix):
        self.create_bucket_and_upload_file("empty_file", s3, kikimr)
        connection_id = client.create_storage_connection(
            unique_prefix + "empty_file_connection", "fbucket"
        ).result.connection_id
        col = ydb.Column(name="data", type=ydb.Type(type_id=ydb.Type.PrimitiveTypeId.STRING))
        binding_name = unique_prefix + "empty_file_binding"
        client.create_object_storage_binding(
            name=binding_name, path="empty_file_path/", format="raw", connection_id=connection_id, columns=[col]
        )

        sql = f'''
            INSERT INTO bindings.`{binding_name}`
            SELECT "" AS data;
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        read_query_id = client.create_query(
            "simple", f"SELECT * FROM bindings.`{binding_name}`", type=fq.QueryContent.QueryType.ANALYTICS
        ).result.query_id
        client.wait_query_status(read_query_id, fq.QueryMeta.COMPLETED)

        data = client.get_result_data(read_query_id)
        result_set = data.result.result_set
        logging.debug(str(result_set))
        assert len(result_set.columns) == 1
        assert result_set.columns[0].name == "data"
        assert result_set.columns[0].type.type_id == ydb.Type.STRING
        assert len(result_set.rows) == 1
        assert result_set.rows[0].items[0].text_value == ""

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_insert_without_format_error(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = f'''
            insert into `{storage_connection_name}`.`/test/`
            select * from AS_TABLE([<|foo:123, bar:"xxx"u|>,<|foo:456, bar:"yyy"u|>]);
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.FAILED)
        issues = str(client.describe_query(query_id).result.query.issue)

        assert "No write format specified. Please use WITH FORMAT for writing into S3" in issues, "Incorrect Issues: " + issues

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_raw_format_validation(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = f'''
            INSERT INTO `{storage_connection_name}`.`/test/`
            WITH (FORMAT = 'raw')
            SELECT * FROM AS_TABLE([<|foo:123, bar:"xxx"u|>,<|foo:456, bar:"yyy"u|>]);
        '''
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.FAILED)
        issues = str(client.describe_query(query_id).result.query.issue)
        assert "Only one column in schema supported in raw format" in issues, "Incorrect Issues: " + issues

        sql = f'''
            INSERT INTO `{storage_connection_name}`.`/test/`
            WITH (FORMAT = 'raw')
            SELECT * FROM AS_TABLE([<|foo:<|bar:"xxx"u|>|>,<|foo:<|bar:"yyy"u|>|>]);
        '''
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.FAILED)
        issues = str(client.describe_query(query_id).result.query.issue)
        assert "Only a column with a primitive type is allowed for the raw format" in issues, "Incorrect Issues: " + issues

    def get_insert_test_query(self, insert_path: str):
        sql = f"INSERT INTO {insert_path} WITH (FORMAT = \"parquet\") SELECT\n"
        for val, type in [
            (2, "Int8"), (-3, "Int16"), (4, "Int32"), (-5, "Int64"), (6, "Uint8"), (7, "Uint16"), (8, "Uint32"), (9, "Uint64"),
            (1, "Bool"),  (10.5, "Float"), (11.9, "Double"),
            ("\"STR_1\"", "String"), ("\"STR_2\"", "Utf8"), ("\"\\\"STR_3\\\"\"", "Json"),
            ("CurrentUtcDate()", "Date"), ("\"2025-05-09T18:44:58Z\"", "Datetime"), ("\"2025-05-09T18:44:58.678906Z\"", "Timestamp"),
            ("CurrentTzDate(1)", "TzDate"), ("\"2025-05-09T21:45:58,Europe/Moscow\"", "TzDateTime"), ("\"2025-05-09T21:45:58.222886,Europe/Moscow\"", "TzTimestamp"),
        ]:
            sql += f"Unwrap(CAST({val} AS {type})) AS `{type}Col`,\n"
            sql += f"Just(Unwrap(CAST({val} AS {type}))) AS `Opt{type}Col`,\n"
            sql += f"CAST(NULL AS {type}) AS `Null{type}Col`,\n"

        logging.debug(sql)
        return sql

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_block_insert_enable(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = self.get_insert_test_query(f"`{storage_connection_name}`.`/test/`")
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        ast = str(client.describe_query(query_id).result.query.ast)
        assert "S3SinkOutput" not in ast, "Incorrect ast: " + ast
        assert "WideToBlocks" in ast, "Incorrect ast: " + ast

        sql = f'''
            SELECT
                COUNT(*)
            FROM `{storage_connection_name}`.`/test/` WITH (
                FORMAT = "parquet",
                SCHEMA (
                    Int8Col Int8,
                )
            )
        '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        result_set = client.get_result_data(query_id).result.result_set
        assert len(result_set.columns) == 1
        assert len(result_set.rows) == 1
        assert result_set.rows[0].items[0].uint64_value == 1

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    def test_block_insert_value(self, kikimr, s3, client, unique_prefix):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = "PRAGMA s3.UseBlocksSink = \"true\";\n" + self.get_insert_test_query(f"`{storage_connection_name}`.`/block/`")
        client.wait_query_status(client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id, fq.QueryMeta.COMPLETED)

        sql = "PRAGMA s3.UseBlocksSink = \"false\";\n" + self.get_insert_test_query(f"`{storage_connection_name}`.`/scalar/`")
        client.wait_query_status(client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id, fq.QueryMeta.COMPLETED)

        sql = f'''
            SELECT
                *
            FROM `{storage_connection_name}`.`/` WITH (
                FORMAT = "raw",
                SCHEMA (
                    Data String,
                )
            )
        '''
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)

        result_set = client.get_result_data(query_id).result.result_set
        assert len(result_set.columns) == 1
        assert len(result_set.rows) == 2
        assert result_set.rows[0].items[0].bytes_value == result_set.rows[1].items[0].bytes_value

    @yq_all
    @pytest.mark.parametrize("client", [{"folder_id": "my_folder"}], indirect=True)
    @pytest.mark.parametrize("block_sink", ["false", "true"])
    def test_insert_deadlock(self, kikimr, s3, client, unique_prefix, block_sink):
        resource = boto3.resource(
            "s3", endpoint_url=s3.s3_url, aws_access_key_id="key", aws_secret_access_key="secret_key"
        )

        bucket = resource.Bucket("insert_bucket")
        bucket.create(ACL='public-read-write')
        bucket.objects.all().delete()

        storage_connection_name = unique_prefix + "ibucket"
        client.create_storage_connection(storage_connection_name, "insert_bucket")

        sql = f'''
            pragma s3.UseBlocksSink = "{block_sink}";
            pragma s3.AtomicUploadCommit = "true";
            pragma s3.InFlightMemoryLimit = "1";

            insert into `{storage_connection_name}`.`/test/`
            with (format = "parquet")
            select Random(data) from AS_TABLE(ListReplicate(<|data:"x"u|>, 1000000));
            '''

        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.FAILED)
        issues = str(client.describe_query(query_id).result.query.issue)

        assert "Writing deadlock occurred, please increase write actor memory limit" in issues, "Incorrect Issues: " + issues
