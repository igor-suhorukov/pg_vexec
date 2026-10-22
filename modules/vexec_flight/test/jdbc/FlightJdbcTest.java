// SPDX-License-Identifier: Apache-2.0
//
// The Flight SQL JDBC driver against vexec_flight (pg_vector_executor.md §5,
// V10): catalogs, schemas and tables listed through DatabaseMetaData, a
// query read through a ResultSet, and DDL and an INSERT through a Statement
// and a PreparedStatement, each run once.  Run by run.sh, its arguments the
// Flight port, the user, the password and the database.

import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

public class FlightJdbcTest {
	static int failed = 0;

	static void check(String what, boolean ok, Object got) {
		System.out.println("  " + (ok ? "ok" : "FAILED") + " jdbc " + what + (ok ? "" : ": " + got));
		if (!ok)
			failed++;
	}

	interface Update {
		Object run() throws SQLException;
	}

	// An update's count, or its error: a check of it fails rather than ending the test.
	static Object update(Update u) {
		try {
			return u.run();
		} catch (SQLException | RuntimeException e) {
			return e.toString();
		}
	}

	static List<String> column(ResultSet rs, String name) throws Exception {
		List<String> out = new ArrayList<>();
		while (rs.next())
			out.add(rs.getString(name));
		rs.close();
		return out;
	}

	public static void main(String[] args) throws Exception {
		String url = "jdbc:arrow-flight-sql://127.0.0.1:" + args[0] + "/?useEncryption=false&database=" + args[3];
		Properties props = new Properties();
		props.setProperty("user", args[1]);
		props.setProperty("password", args[2]);
		try (Connection conn = DriverManager.getConnection(url, props)) {
			DatabaseMetaData md = conn.getMetaData();
			List<String> catalogs = column(md.getCatalogs(), "TABLE_CAT");
			check("lists the catalogs", catalogs.contains(args[3]), catalogs);
			List<String> schemas = column(md.getSchemas(args[3], "pub%"), "TABLE_SCHEM");
			check("lists the schemas", schemas.contains("public"), schemas);
			List<String> tables = column(md.getTables(args[3], "public", "corpus", null), "TABLE_NAME");
			check("lists the tables", tables.size() == 1 && tables.get(0).equals("corpus"), tables);
			List<String> columns = column(md.getColumns(args[3], "public", "corpus", "n10%"), "COLUMN_NAME");
			check("lists a table's columns", columns.contains("n10_2"), columns);
			try (Statement st = conn.createStatement();
				 ResultSet rs = st.executeQuery("SELECT id, i4, tx, n10_2 FROM corpus WHERE id <= 100 ORDER BY id")) {
				int rows = 0;
				long ids = 0;
				while (rs.next()) {
					rows++;
					ids += rs.getInt("id");
				}
				check("reads a query's rows", rows == 100 && ids == 5050, rows + " rows, ids " + ids);
			}
			// DDL counts 0 rows: the driver takes an update count of -1 for a query's,
			// and asks for its result by GetFlightInfo, which would run it again
			try (Statement st = conn.createStatement()) {
				Object got = update(() -> st.execute("CREATE TEMP TABLE jdbc_ddl (i int4)") ? "a result set" : st.getUpdateCount());
				check("runs DDL once, counting 0 rows", Integer.valueOf(0).equals(got), got);
				got = update(() -> st.executeUpdate("INSERT INTO jdbc_ddl VALUES (1), (2), (3)"));
				check("counts an INSERT's rows", Integer.valueOf(3).equals(got), got);
			}
			try (PreparedStatement ps = conn.prepareStatement("CREATE TEMP TABLE jdbc_ddl_prepared (i int4)")) {
				Object got = update(ps::executeUpdate);
				check("runs a prepared DDL once, counting 0 rows", Integer.valueOf(0).equals(got), got);
			}
			try (Statement st = conn.createStatement();
				 ResultSet rs = st.executeQuery("SELECT (SELECT count(*) FROM jdbc_ddl) + (SELECT count(*) FROM jdbc_ddl_prepared) AS n")) {
				rs.next();
				check("reads what the updates made", rs.getLong("n") == 3, rs.getLong("n"));
			}
		}
		System.out.println(failed == 0 ? "jdbc: every check passed" : "jdbc: " + failed + " checks FAILED");
		System.exit(failed == 0 ? 0 : 1);
	}
}
