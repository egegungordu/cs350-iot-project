var AWS = require("aws-sdk");
const webpush = require('web-push');
var fs = require("fs");
let path = require("path");
const dynamodb = new AWS.DynamoDB();

webpush.setVapidDetails(
    process.env['PushSubject'],
    process.env['PushPublicKey'],
    process.env['PushPrivateKey']
);

function loadFile(templateName) {
  const fileName = `./${templateName}`
  let resolved
  if (process.env.LAMBDA_TASK_ROOT) {
    resolved = path.resolve(process.env.LAMBDA_TASK_ROOT, fileName)
  } else {
    resolved = path.resolve(__dirname, fileName)
  }
  console.log(`Loading file at: ${resolved}`)
  try {
    const data = fs.readFileSync(resolved, 'utf8')
    return data
  } catch (error) {
    const message = `Could not load file at: ${resolved}, error: ${JSON.stringify(error, null, 2)}`
    console.error(message)
    throw new Error(message)
  }
}

const getSubscriptions = async (topic) => {
    var params = {
        ExpressionAttributeValues: { ":topic": { S: topic } },
        KeyConditionExpression: "topic = :topic",
        ProjectionExpression: "subscription",
        TableName: process.env['SubscriptionsTable']
    };

    return new Promise((ok, err) => {
        dynamodb.query(params, function (e, data) {
            if (e) err(e);
            else ok(data);
        })
    })
}

const putSubscription = async (topic, subscription) => {
    var params = {
      TableName: process.env['SubscriptionsTable'],
      Item: {
        'topic' : {S: topic},
        'subscription' : {S: subscription}
      }
    }
    
    return new Promise((ok, err) => {
        dynamodb.putItem(params, function(e, data) {
            if (e) err(e);
            else ok(data);
        })
    })
}

const deleteSubscription = async (topic, subscription) => {
    var params = {
      TableName: process.env['SubscriptionsTable'],
      Key: {
        'topic' : {S: topic},
        'subscription' : {S: subscription}
      }
    }
    
    return new Promise((ok, err) => {
        dynamodb.deleteItem(params, function(e, data) {
            if (e) err(e);
            else ok(data);
        })
    })
}

const subscribePage = () => {
    return {
        statusCode: 200,
        headers: {
            'Content-Type': 'text/html'
        },
        body: loadFile("index.html")
    }   
}

const serviceWorkerPage = () => {
    return {
        statusCode: 200,
        headers: {
            'Content-Type': 'text/javascript',
        },
        body: 
            'self.addEventListener("push", function (event) { \
                const message = event.data.json(); \
                self.registration.showNotification( message.title, { body: message.text }); \
            })', 
    };
}

const notify = async (event) => {
    const { topic, title, text, password } = JSON.parse(event.body);
    if (password != process.env['Password']) {
        return {
            statusCode: 401,
            headers: {
                'Content-Type': 'text/plain',
            },
            body: `Unauthorized`, 
        };
    }
    const { Items } = await getSubscriptions(topic);
    const promises = [];
    Items.forEach(d => {
        const subscription = JSON.parse(d.subscription.S);
        let result = webpush.sendNotification(subscription, JSON.stringify({ title, text }))
            .catch((e) => {
                if(e.statusCode == 410) {
                    deleteSubscription(topic, d.subscription.S)
                }
            })
        promises.push(result);
    });
    
    await Promise.all(promises);
    return {
        statusCode: 201,
        headers: {
            'Content-Type': 'text/plain',
        },
        body: `${Items.length}`, 
    };
}

const confirmSubscribe = async (event) => {
    const { topic, subscription } = JSON.parse(event.body)
    const response = await putSubscription(topic, subscription)
    return {
        statusCode: 201,
        headers: {
            'Content-Type': 'application/json',
        },
        body: JSON.stringify(response), 
    };
}

exports.handler = async (event, context) => {
    switch (event.rawPath) {
        case '/default/IoTsubscribe':
            return subscribePage()
        case '/default/sw.js':
            return serviceWorkerPage()
        case '/default/notify':
            return await notify(event)
        case '/default/confirmSubscribe':
            return await confirmSubscribe(event)
        default:
            return {
                statusCode: 200,
                headers: {
                    'Content-Type': 'text/html',
                },
                body: 
                    '<p>no</p>', 
            };
    }
};