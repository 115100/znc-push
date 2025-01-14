/**
 * ZNC Push Module
 *
 * Allows the user to enter a Push user and API token, and sends
 * channel highlights and personal messages to Push.
 *
 * Copyright (c) 2011 John Reese
 * Licensed under the MIT license
 */

#define REQUIRESSL
#ifndef PUSHVERSION
#define PUSHVERSION "dev"
#endif

#include <znc/znc.h>
#include <znc/Chan.h>
#include <znc/User.h>
#include <znc/IRCNetwork.h>
#include <znc/Modules.h>
#include <znc/FileUtils.h>
#include <znc/Client.h>
#include "time.h"
#include <string.h>

#include <curl/curl.h>

// Forward declaration
class CPushMod;

/**
 * Shorthand for encoding a string for a URL.
 *
 * @param str String to be encoded
 * @return Encoded string
 */
CString urlencode(const CString& str)
{
	return str.Escape_n(CString::EASCII, CString::EURL);
}

// forward declaration
long make_curl_request(const CString& service_host, const CString& service_url,
						   const CString& service_auth, MCString& params, int port,
						   bool use_ssl, bool use_post,
						   const CString& proxy, bool proxy_ssl_verify,
						   bool debug);

/**
 * Push notification module.
 */
class CPushMod : public CModule
{
	protected:

		// Application name
		CString app;

		// Time last notification was sent for a given context
		std::map <CString, time_t> last_notification_time;

		// Time of last message by user to a given context
		std::map <CString, time_t> last_reply_time;

		// Time of last activity by user for a given context
		std::map <CString, time_t> last_active_time;

		// Time of last activity by user in any context
		time_t idle_time;

		// User object
		CUser *user;

		// Configuration options
		MCString options;
		MCString defaults;

	public:

		MODCONSTRUCTOR(CPushMod) {
			curl_global_init(CURL_GLOBAL_DEFAULT);

			app = "ZNC";

			idle_time = time(NULL);

			// Current user
			user = GetUser();

			// Push service information
			defaults["username"] = "";
			defaults["secret"] = "";
			defaults["target"] = "";

			// Igloo Devices
			defaults["device1"] = "";
			defaults["device2"] = "";
			defaults["device3"] = "";
			defaults["device4"] = "";
			defaults["device5"] = "";

			// Notification settings
			defaults["message_content"] = "{context}: [{nick}] {message}";
			defaults["message_length"] = "100";
			defaults["message_title"] = "{title}";
			defaults["message_uri"] = "";
			defaults["message_uri_post"] = "no";
			defaults["message_uri_title"] = "";
			defaults["message_priority"] = "0";
			defaults["message_sound"] = "";
			defaults["message_escape"] = "";

			// Notification conditions
			defaults["away_only"] = "no";
			defaults["client_count_less_than"] = "0";
			defaults["highlight"] = "";
			defaults["idle"] = "0";
			defaults["last_active"] = "180";
			defaults["last_notification"] = "300";
			defaults["nick_blacklist"] = "";
			defaults["network_blacklist"] = "";
			defaults["replied"] = "yes";
			defaults["context"] = "*";

			// Proxy, for libcurl
			defaults["proxy"] = "";
			defaults["proxy_ssl_verify"] = "yes";

			// Advanced
			defaults["channel_conditions"] = "all";
			defaults["query_conditions"] = "all";
		}

		virtual ~CPushMod() {
			curl_global_cleanup();
		}

	protected:

		/**
		 * Performs string expansion on a set of keywords.
		 * Given an initial string and a dictionary of string replacments,
		 * iterate over the dictionary, expanding keywords one-by-one.
		 *
		 * @param content String contents
		 * @param replace Dictionary of string replacements
		 * @return Result of string replacements
		 */
		CString expand(const CString& content, MCString& replace)
		{
			CString result = content.c_str();

			for(MCString::iterator i = replace.begin(); i != replace.end(); i++)
			{
				result.Replace(i->first, i->second);
			}

			return result;
		}

		/**
		 * Verifies whether a given string contains only numbers.
		 *
		 * @param content String to verify
		 */
		bool is_number(const CString& content)
		{
			CString::const_iterator it = content.begin();
			while(it != content.end() && std::isdigit(*it)) ++it;
			return !content.empty() && it == content.end();
		}

		/**
		 * Send a message to the currently-configured push service.
		 * Requires (and assumes) that the user has already configured their
		 * username and API secret using the 'set' command.
		 *
		 * @param message Message to be sent to the user
		 * @param title Message title to use
		 * @param context Channel or nick context
		 */
		void send_message(const CString& message, const CString& title="New Message", const CString& context="*push", const CNick& nick=CString("*push"), const CString& type="")
		{
			// Set the last notification time
			last_notification_time[context] = time(NULL);

			// Shorten message if needed
			unsigned int message_length = options["message_length"].ToUInt();
			CString short_message = message;
			if (message_length > 0)
			{
				short_message = message.Ellipsize(message_length);
			}

			// Generate an ISO8601 date string
			time_t rawtime;
			struct tm * timeinfo;
			time(&rawtime);
			timeinfo = localtime(&rawtime);
			char iso8601 [20];
			strftime(iso8601, 20, "%Y-%m-%d %H:%M:%S", timeinfo);

			// Message string replacements
			MCString replace;
			replace["{context}"] = context;
			replace["{nick}"] = nick.GetNick();
			replace["{datetime}"] = CString(iso8601);
			replace["{unixtime}"] = CString(time(NULL));
			replace["{message}"] = short_message;
			replace["{title}"] = title;
			replace["{username}"] = options["username"];
			replace["{secret}"] = options["secret"];
			replace["{target}"] = options["target"];
			// network is special because it can be nullptr if the user has none set up yet
			CIRCNetwork* network = GetNetwork();
			if (network) {
				replace["{network}"] = network->GetName();
			} else {
				replace["{network}"] = "(No network)";
			}

			if (options["message_escape"] != "")
			{
				CString::EEscape esc = CString::ToEscape(options["message_escape"]);
				for (MCString::iterator i = replace.begin(); i != replace.end(); i++) {
					i->second = i->second.Escape(esc);
				}
			}

			CString message_uri = expand(options["message_uri"], replace);
			CString message_title = expand(options["message_title"], replace);
			CString message_content = expand(options["message_content"], replace);

			// Set up the connection profile
			bool use_post = true;
			int use_port = 443;
			bool use_ssl = true;
			CString service_host;
			CString service_url;
			CString service_auth;
			MCString params;

			if (options["device1"] == "" && options["device2"] == "" && options["device3"] == "" && options["device4"] == "" && options["device5"] == "")
			{
				PutModule("Error: no devices are set");
				return;
			}

			service_host = "api.iglooirc.com";
			service_url = "/znc/push";

			params["network"] = network->GetName();
			params["channel"] = context;
			params["message"] = message;
			params["nick"] = network->GetNick();
			params["sender"] = nick.GetNick();
			params["type"] = type;
			//params["developer"] = "true";

			params["device1"] = options["device1"];
			params["device2"] = options["device2"];
			params["device3"] = options["device3"];
			params["device4"] = options["device4"];
			params["device5"] = options["device5"];

			long http_code = make_curl_request(service_host, service_url, service_auth, params, use_port, use_ssl, use_post, options["proxy"], options["proxy_ssl_verify"] != "no", false);
			if (!(http_code >= 200 && http_code < 300)) {
				PutModule("Error: HTTP status code " + CString(http_code));
			}
		}

		/**
		 * Evaluate a boolean expression using condition values.
		 * All tokens must be separated by spaces, using "and" and "or" for
		 * boolean operators, "(" and ")" to enclose sub-expressions, and
		 * condition option names to evaluate each condition.
		 *
		 * @param expression Boolean expression string
		 * @param context Notification context
		 * @param nick Sender nick
		 * @param message Message contents
		 * @return Result of boolean evaluation
		 */
		bool eval(const CString& expression, const CString& context=CString(""), const CNick& nick=CNick(""), const CString& message=" ")
		{
			CString padded = expression.Replace_n("(", " ( ");
			padded.Replace(")", " ) ");

			VCString tokens;
			padded.Split(" ", tokens, false);

			bool result = eval_tokens(tokens.begin(), tokens.end(), context, nick, message);

			return result;
		}

#define expr(x, y) else if (token == x) { \
	bool result = y; \
	dbg += CString(x) + "/" + CString(result ? "true" : "false") + " "; \
	value = oper ? value && result : value || result; \
}

		/**
		 * Evaluate a tokenized boolean expression, or sub-expression.
		 *
		 * @param pos Token vector iterator current position
		 * @param end Token vector iterator end position
		 * @param context Notification context
		 * @param nick Sender nick
		 * @param message Message contents
		 * @return Result of boolean expression
		 */
		bool eval_tokens(VCString::iterator pos, VCString::iterator end, const CString& context, const CNick& nick, const CString& message)
		{
			bool oper = true;
			bool value = true;

			CString dbg = "";

			for(; pos != end; pos++)
			{
				CString token = pos->AsLower();

				if (token == "(")
				{
					// recursively evaluate sub-expressions
					bool inner = eval_tokens(++pos, end, context, nick, message);
					dbg += "( inner/" + CString(inner ? "true" : "false") + " ) ";
					value = oper ? value && inner : value || inner;

					// search ahead to the matching parenthesis token
					unsigned int parens = 1;
					while(pos != end)
					{
						if (*pos == "(")
						{
							parens++;
						}
						else if (*pos == ")")
						{
							parens--;
						}

						if (parens == 0)
						{
							break;
						}

						pos++;
					}
				}
				else if (token == ")")
				{
					pos++;
					return value;
				}
				else if (token == "and")
				{
					dbg += "and ";
					oper = true;
				}
				else if (token == "or")
				{
					dbg += "or ";
					oper = false;
				}

				expr("true", true)
				expr("false", false)
				expr("away_only", away_only())
				expr("client_count_less_than", client_count_less_than())
				expr("highlight", highlight(message))
				expr("idle", idle())
				expr("last_active", last_active(context))
				expr("last_notification", last_notification(context))
				expr("nick_blacklist", nick_blacklist(nick))
				expr("network_blacklist", network_blacklist())
				expr("replied", replied(context))
				expr("context", context_filter(context))

				else
				{
					PutModule("Error: Unexpected token \"" + token + "\"");
				}
			}

			return value;
		}

#undef expr

	protected:

		/**
		 * Check if the away status condition is met.
		 *
		 * @return True if away_only is not "yes" or away status is set
		 */
		bool away_only()
		{
			CString value = options["away_only"].AsLower();
			return value != "yes" || GetNetwork()->IsIRCAway();
		}

		/**
		 * Check how many clients are connected to ZNC.
		 *
		 * @return Number of connected clients
		 */
		size_t client_count()
		{
			return GetNetwork()->GetClients().size();
		}

		/**
		 * Check if the client_count condition is met.
		 *
		 * @return True if client_count is less than client_count_less_than or if client_count_less_than is zero
		 */
		bool client_count_less_than()
		{
			unsigned int value = options["client_count_less_than"].ToUInt();
			return value == 0 || client_count() < value;
		}

		/**
		 * Determine if the given message matches any highlight rules.
		 *
		 * @param message Message contents
		 * @return True if message matches a highlight
		 */
		bool highlight(const CString& message)
		{
			CString msg = " " + message.AsLower() + " ";

			VCString values;
			options["highlight"].Split(" ", values, false);
			values.push_back("%nick%");

			for (VCString::iterator i = values.begin(); i != values.end(); i++)
			{
				CString value = i->AsLower();
				char prefix = value[0];
				bool negate_match = false;

				if (prefix == '-')
				{
					negate_match = true;
					value.LeftChomp(1);
				}
				else if (prefix == '_')
				{
					value = " " + value.LeftChomp_n(1) + " ";
				}

				// Expand substrings like %nick%
				if (m_pNetwork)
				{
					value = m_pNetwork->ExpandString(value);
				}
				else
				{
					value = GetUser()->ExpandString(value);
				}

				value = "*" + value.AsLower() + "*";

				if (msg.WildCmp(value))
				{
					return !negate_match;
				}
			}

			return false;
		}

		/**
		 * Determine if the given context matches any context rules.
		 *
		 * @param context The context of a message
		 * @return True if context matches the filter
		 */
		bool context_filter(const CString& raw_context)
		{
			CString context = raw_context.AsLower();

			if (context == "all" || context == "*")
				return true;

			VCString values;
			options["context"].Split(" ", values, false);

			for (VCString::iterator i = values.begin(); i != values.end(); i++)
			{
				CString value = i->AsLower();
				char prefix = value[0];
				bool push = true;

				if (prefix == '-')
				{
					push = false;
					value.LeftChomp(1);
				}

				if (value != "*")
				{
					value = "*" + value.AsLower() + "*";
				}

				if (context.WildCmp(value))
				{
					return push;
				}
			}

			return false;
		}


		/**
		 * Check if the idle condition is met.
		 *
		 * @return True if idle is less than or equal to zero or elapsed time is greater than idle
		 */
		bool idle()
		{
			unsigned int value = options["idle"].ToUInt();
			time_t now = time(NULL);
			return value == 0 || difftime(now, idle_time) >= value;
		}

		/**
		 * Check if the last_active condition is met.
		 *
		 * @param context Channel or nick context
		 * @return True if last_active is less than or equal to zero or elapsed time is greater than last_active
		 */
		bool last_active(const CString& context)
		{
			unsigned int value = options["last_active"].ToUInt();
			time_t now = time(NULL);
			return value == 0
				|| last_active_time.count(context) < 1
				|| difftime(now, last_active_time[context]) >= value;
		}

		/**
		 * Check if the last_notification condition is met.
		 *
		 * @param context Channel or nick context
		 * @return True if last_notification is less than or equal to zero or elapsed time is greater than last_nofication
		 */
		bool last_notification(const CString& context)
		{
			unsigned int value = options["last_notification"].ToUInt();
			time_t now = time(NULL);
			return value == 0
				|| last_notification_time.count(context) < 1
				|| difftime(now, last_notification_time[context]) >= value;
		}

		/**
		 * Check if the nick_blacklist condition is met.
		 *
		 * @param nick Nick that sent the message
		 * @return True if nick is not in the blacklist
		 */
		bool nick_blacklist(const CNick& nick)
		{
			VCString blacklist;
			options["nick_blacklist"].Split(" ", blacklist, false);

			CString name = nick.GetNick().AsLower();

			for (VCString::iterator i = blacklist.begin(); i != blacklist.end(); i++)
			{
				CString value;

				// Expand substrings like %nick%
				if (m_pNetwork)
				{
					value = m_pNetwork->ExpandString(*i);
				}
				else
				{
					value = GetUser()->ExpandString(*i);
				}

				if (name.WildCmp(value.AsLower()))
				{
					return false;
				}
			}

			return true;
		}

		/**
		 * Check if the network_blacklist condition is met.
		 *
		 * @param network Network that the message was received on
		 * @return True if network is not in the blacklist
		 */
		bool network_blacklist()
		{
			VCString blacklist;
			options["network_blacklist"].Split(" ", blacklist, false);

			CString name = (*m_pNetwork).GetName().AsLower();

			for (VCString::iterator i = blacklist.begin(); i != blacklist.end(); i++)
			{
				if (name.WildCmp((*i).AsLower()))
				{
					return false;
				}
			}

			return true;
		}

		/**
		 * Check if the replied condition is met.
		 *
		 * @param context Channel or nick context
		 * @return True if last_reply_time > last_notification_time or if replied is not "yes"
		 */
		bool replied(const CString& context)
		{
			CString value = options["replied"].AsLower();
			return value != "yes"
				|| last_notification_time[context] == 0
				|| last_notification_time[context] < last_reply_time[context];
		}

		/**
		 * Determine when to notify the user of a channel message.
		 *
		 * @param nick Nick that sent the message
		 * @param channel Channel the message was sent to
		 * @param message Message contents
		 * @return Notification should be sent
		 */
		bool notify_channel(const CNick& nick, const CChan& channel, const CString& message)
		{
			CString context = channel.GetName();

			CString expression = options["channel_conditions"].AsLower();
			if (expression != "all")
			{
				return eval(expression, context, nick, message);
			}

			return !channel.IsDetached()
				&& away_only()
				&& client_count_less_than()
				&& highlight(message)
				&& idle()
				&& last_active(context)
				&& last_notification(context)
				&& nick_blacklist(nick)
				&& network_blacklist()
				&& replied(context)
				&& context_filter(context);
		}

		/**
		 * Determine when to notify the user of a private message.
		 *
		 * @param nick Nick that sent the message
		 * @return Notification should be sent
		 */
		bool notify_pm(const CNick& nick, const CString& message)
		{
			CString context = nick.GetNick();

			CString expression = options["query_conditions"].AsLower();
			if (expression != "all")
			{
				return eval(expression, context, nick, message);
			}

			return away_only()
				&& client_count_less_than()
				&& idle()
				&& last_active(context)
				&& last_notification(context)
				&& nick_blacklist(nick)
				&& network_blacklist()
				&& replied(context)
				&& true;
		}

	protected:

		/**
		 * Handle the plugin being loaded.  Retrieve plugin config values.
		 *
		 * @param args Plugin arguments
		 * @param message Message to show the user after loading
		 */
		bool OnLoad(const CString& args, CString& message)
		{
			for (MCString::iterator i = defaults.begin(); i != defaults.end(); i++)
			{
				CString value = GetNV(i->first);
				if (value != "")
				{
					options[i->first] = value;
				}
				else
				{
					options[i->first] = defaults[i->first];
				}
			}

			return true;
		}

		/**
		 * Handle channel messages.
		 *
		 * @param nick Nick that sent the message
		 * @param channel Channel the message was sent to
		 * @param message Message contents
		 */
		EModRet OnChanMsg(CNick& nick, CChan& channel, CString& message)
		{
			if (notify_channel(nick, channel, message))
			{
				CString title = "Highlight";

				send_message(message, title, channel.GetName(), nick);
			}

			return CONTINUE;
		}

		/**
		 * Handle channel actions.
		 *
		 * @param nick Nick that sent the action
		 * @param channel Channel the message was sent to
		 * @param message Message contents
		 */
		EModRet OnChanAction(CNick& nick, CChan& channel, CString& message)
		{
			if (notify_channel(nick, channel, message))
			{
				CString title = "Highlight";

				send_message(message, title, channel.GetName(), nick, "action");
			}

			return CONTINUE;
		}

		/**
		 * Handle channel notices.
		 *
		 * @param nick Nick that sent the notice
		 * @param channel Channel the notice was sent to
		 * @param message Notice contents
		 */
		EModRet OnChanNotice(CNick& nick, CChan& channel, CString& message)
		{
			if (notify_channel(nick, channel, message))
			{
				CString title = "Channel Notice";

				send_message(message, title, channel.GetName(), nick, "notice");
			}

			return CONTINUE;
		}

		/**
		 * Handle a private message.
		 *
		 * @param nick Nick that sent the message
		 * @param message Message contents
		 */
		EModRet OnPrivMsg(CNick& nick, CString& message)
		{
			if (notify_pm(nick, message))
			{
				CString title = "Private Message";

				send_message(message, title, nick.GetNick(), nick);
			}

			return CONTINUE;
		}

		/**
		 * Handle a private action.
		 *
		 * @param nick Nick that sent the action
		 * @param message Message contents
		 */
		EModRet OnPrivAction(CNick& nick, CString& message)
		{
			if (notify_pm(nick, message))
			{
				CString title = "Private Message";

				send_message(message, title, nick.GetNick(), nick, "action");
			}

			return CONTINUE;
		}

		/**
		 * Handle a private notice.
		 *
		 * @param nick Nick that sent the notice
		 * @param message Notice contents
		 */
		EModRet OnPrivNotice(CNick& nick, CString& message)
		{
			if (notify_pm(nick, message))
			{
				CString title = "Private Notice";

				send_message(message, title, nick.GetNick(), nick, "notice");
			}

			return CONTINUE;
		}

		/**
		 * Handle a message sent by the user.
		 *
		 * @param target Target channel or nick
		 * @param message Message contents
		 */
		EModRet OnUserMsg(CString& target, CString& message)
		{
			last_reply_time[target] = last_active_time[target] = idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle an action sent by the user.
		 *
		 * @param target Target channel or nick
		 * @param message Message contents
		 */
		EModRet OnUserAction(CString& target, CString& message)
		{
			last_reply_time[target] = last_active_time[target] = idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle a notice sent by the user.
		 *
		 * @param target Target channel or nick
		 * @param message Notice contents
		 */
		EModRet OnUserNotice(CString& target, CString& message)
		{
			last_reply_time[target] = last_active_time[target] = idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle the user joining a channel.
		 *
		 * @param channel Channel name
		 * @param key Channel key
		 */
		EModRet OnUserJoin(CString& channel, CString& key)
		{
			idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle the user parting a channel.
		 *
		 * @param channel Channel name
		 * @param message Part message
		 */
		EModRet OnUserPart(CString& channel, CString& message)
		{
			idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle the user setting the channel topic.
		 *
		 * @param channel Channel name
		 * @param topic Topic message
		 */
		EModRet OnUserTopic(CString& channel, CString& topic)
		{
			idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle the user requesting the channel topic.
		 *
		 * @param channel Channel name
		 */
		EModRet OnUserTopicRequest(CString& channel)
		{
			idle_time = time(NULL);
			return CONTINUE;
		}

		/**
		 * Handle direct commands to the *push virtual user.
		 *
		 * @param command Command string
		 */
		void OnModCommand(const CString& command)
		{
			VCString tokens;
			CString::size_type token_count = command.Split(" ", tokens, false);

			if (token_count < 1)
			{
				return;
			}

			CString action = tokens[0].AsLower();

			// SET command
			if (action == "set")
			{
				if (token_count < 3)
				{
					PutModule("Usage: set <option> <value>");
					return;
				}

				CString option = tokens[1].AsLower();
				CString value = command.Token(2, true, " ");
				MCString::iterator pos = options.find(option);

				if (pos == options.end())
				{
					PutModule("Error: invalid option name");
				}
				else
				{
					value.Trim();

					if (option == "channel_conditions" || option == "query_conditions")
					{
						if (value != "all")
						{
							eval(value);
						}
					}

					options[option] = value;
					SetNV(option, options[option]);

					PutModule("Ok");
				}
			}
			// APPEND command
			else if (action == "append")
			{
				if (token_count < 3)
				{
					PutModule("Usage: append <option> <value>");
					return;
				}

				CString option = tokens[1].AsLower();
				CString value = command.Token(2, true, " ");
				MCString::iterator pos = options.find(option);

				if (pos == options.end())
				{
					PutModule("Error: invalid option name");
				}
				else
				{
					options[option] += " " + value;
					options[option].Trim();
					SetNV(option, options[option]);

					PutModule("Ok");
				}
			}
			// PREPEND command
			else if (action == "prepend")
			{
				if (token_count < 3)
				{
					PutModule("Usage: prepend <option> <value>");
					return;
				}

				CString option = tokens[1].AsLower();
				CString value = command.Token(2, true, " ");
				MCString::iterator pos = options.find(option);

				if (pos == options.end())
				{
					PutModule("Error: invalid option name");
				}
				else
				{
					options[option] = value + " " + options[option];
					options[option].Trim();
					SetNV(option, options[option]);

					PutModule("Ok");
				}
			}
			// UNSET command
			else if (action == "unset")
			{
				if (token_count != 2)
				{
					PutModule("Usage: unset <option>");
					return;
				}

				CString option = tokens[1].AsLower();
				MCString::iterator pos = options.find(option);

				if (pos == options.end())
				{
					PutModule("Error: invalid option name");
				}
				else
				{
					options[option] = defaults[option];
					DelNV(option);

					PutModule("Ok");
				}
			}
			// GET command
			else if (action == "get")
			{
				if (token_count > 2)
				{
					PutModule("Usage: get [<option>]");
					return;
				}

				if (token_count < 2)
				{
					CTable table;

					table.AddColumn("Option");
					table.AddColumn("Value");

					for (MCString::iterator i = options.begin(); i != options.end(); i++)
					{
						table.AddRow();
						table.SetCell("Option", i->first);
						table.SetCell("Value", i->second);
					}

					PutModule(table);
					return;
				}

				CString option = tokens[1].AsLower();
				MCString::iterator pos = options.find(option);

				if (pos == options.end())
				{
					PutModule("Error: invalid option name");
				}
				else
				{
					PutModule(option + CString(": ") + options[option]);
				}
			}
			// SAVE command
			else if (action == "save")
			{
				if (token_count < 2)
				{
					PutModule("Usage: save <filepath>");
				}

				CString file_path = command.Token(1, true, " ");
				int status = options.WriteToDisk(file_path);

				if (status == MCString::MCS_SUCCESS)
				{
					PutModule("Options saved to " + file_path);
				}
				else
				{
					switch (status)
					{
						case MCString::MCS_EOPEN:
						case MCString::MCS_EWRITE:
						case MCString::MCS_EWRITEFIL:
							PutModule("Failed to save options to " + file_path);
							break;
						default:
							PutModule("Failure");
							break;
					}
				}
			}
			// LOAD command
			else if (action == "load")
			{
				if (token_count < 2)
				{
					PutModule("Usage: load <filename>");
				}

				CString file_path = command.Token(1, true, " ");

				if (!CFile::Exists(file_path))
				{
					PutModule("File does not exist: " + file_path);
					return;
				}

				int status = options.ReadFromDisk(file_path);

				if (status == MCString::MCS_SUCCESS)
				{
					PutModule("Options loaded from " + file_path);

					// Restore any defaults that aren't in the loaded dictionary,
					// and save loaded options to ZNC's data store
					for (MCString::iterator i = defaults.begin(); i != defaults.end(); i++)
					{
						CString option = i->first;
						MCString::iterator pos = options.find(option);

						if (pos == options.end())
						{
							options[option] = defaults[option];
							DelNV(option);
						}
						else
						{
							SetNV(option, options[option]);
						}
					}
				}
				else
				{
					switch (status)
					{
						case MCString::MCS_EOPEN:
						case MCString::MCS_EREADFIL:
							PutModule("Failed to read options from " + file_path);
							break;
						default:
							PutModule("Failure");
							break;
					}
				}
			}
			// STATUS command
			else if (action == "status")
			{
				CTable table;

				table.AddColumn("Condition");
				table.AddColumn("Status");

				table.AddRow();
				table.SetCell("Condition", "away");
				table.SetCell("Status", GetNetwork()->IsIRCAway() ? "yes" : "no");

				table.AddRow();
				table.SetCell("Condition", "client_count");
				table.SetCell("Status", CString(client_count()));

				time_t now = time(NULL);
				time_t ago = now - idle_time;

				table.AddRow();
				table.SetCell("Condition", "idle");
				table.SetCell("Status", CString(ago) + " seconds");

				table.AddRow();
				table.SetCell("Condition", "network_blacklist");
				// network_blacklist() is True if the network is not in a blacklist
				table.SetCell("Status", network_blacklist() ? "no" : "yes");

				if (token_count > 1)
				{
					CString context = tokens[1];

					table.AddRow();
					table.SetCell("Condition", "last_active");

					if (last_active_time.count(context) < 1)
					{
						table.SetCell("Status", "n/a");
					}
					else
					{
						ago = now - last_active_time[context];
						table.SetCell("Status", CString(ago) + " seconds");
					}

					table.AddRow();
					table.SetCell("Condition", "last_notification");

					if (last_notification_time.count(context) < 1)
					{
						table.SetCell("Status", "n/a");
					}
					else
					{
						ago = now - last_notification_time[context];
						table.SetCell("Status", CString(ago) + " seconds");
					}

					table.AddRow();
					table.SetCell("Condition", "replied");
					table.SetCell("Status", replied(context) ? "yes" : "no");
				}

				PutModule(table);
			}
			// SUBSCRIBE command
			else if (action == "subscribe")
			{
				// Set up the connection profile
				bool use_post = true;
				int use_port = 443;
				bool use_ssl = true;
				CString service_host;
				CString service_url;
				CString service_auth;
				MCString params;

				long http_code = make_curl_request(service_host, service_url, service_auth, params, use_port, use_ssl, use_post, options["proxy"], options["proxy_ssl_verify"] != "no", false);
				if (!(http_code >= 200 && http_code < 300)) {
					PutModule("Error: HTTP status code " + CString(http_code));
				}

				PutModule("Ok");
			}
			// SEND command
			else if (action == "send")
			{
				CString message = command.Token(1, true, " ", true);
				send_message(message);

				PutModule("Ok");
			}
			// HELP command
			else if (action == "help")
			{
				PutModule("View the detailed documentation at https://github.com/jreese/znc-push/blob/master/README.md");
			}
			// VERSION command
			else if (action == "version")
			{
				PutModule("znc-push " + CString(PUSHVERSION));
			}
			// EVAL command
			else if (action == "eval")
			{
				CString value = command.Token(1, true, " ");
				PutModule(eval(value) ? "true" : "false");
			}
			else
			{
				PutModule("Error: invalid command, try `help`");
			}
		}
};

/**
 * Build a query string from a dictionary of request parameters.
 *
 * @param params Request parameters
 * @return query string
 */
CString build_query_string(MCString& params)
{
	bool more = false;
	CString query;
	CString key;
	CString value;
	for (MCString::iterator param = params.begin(); param != params.end(); param++)
	{
		key = urlencode(param->first);
		value = urlencode(param->second);

		if (more)
		{
			query += "&" + key + "=" + value;
		}
		else
		{
			query += key + "=" + value;
			more = true;
		}
	}

	return query;
}

/**
 * Send an HTTP request using libcurl.
 *
 * @param service_host Host domain
 * @param service_url Host path
 * @param service_auth Basic auth string
 * @param params Request parameters
 * @param port Port number
 * @param use_ssl Use SSL
 * @param use_post Use POST method
 */
long make_curl_request(const CString& service_host, const CString& service_url,
						   const CString& service_auth, MCString& params, int port,
						   bool use_ssl, bool use_post,
						   const CString& proxy, bool proxy_ssl_verify,
						   bool debug)
{
	CURL *curl;
	CURLcode result;
	long http_code;

	curl = curl_easy_init();

	CString user_agent = "ZNC Push/" + CString(PUSHVERSION);

	CString url = CString(use_ssl ? "https" : "http") + "://" + service_host + service_url;
	CString query = build_query_string(params);
	if (!query.empty())
	{
		url = url + "?" + query;
	}

	if (debug)
	{
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	}

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	curl_easy_setopt(curl, CURLOPT_URL, url.data());
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3); // three seconds ought to be good enough for anyone, eh?

	if (service_auth != "")
	{
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		curl_easy_setopt(curl, CURLOPT_USERPWD, service_auth.data());
	}

	if (use_post)
	{
		curl_easy_setopt(curl, CURLOPT_POST, 1);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.data());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, query.length());
	}

	if (proxy != "") {
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

		if (!proxy_ssl_verify) {
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		}
	}

	result = curl_easy_perform(curl);
	if (result != CURLE_OK) {
		curl_easy_cleanup(curl);
		return -1;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	return http_code;
}

template<> void TModInfo<CPushMod>(CModInfo& Info) {
	Info.AddType(CModInfo::UserModule);
	Info.SetWikiPage("push");
}

NETWORKMODULEDEFS(CPushMod, "Send highlights and personal messages to a push notification service")
